// Exercise the real POSIX transport's deadlines (#720) without a serial device.
//
// Like serial_port_win32_timeout_test, this compiles SerialPortPosix.cpp into
// the test with three calls redirected: ioctl (so TIOCOUTQ can report output
// that never drains, or fail with EINTR; a pty always reports 0), write(2)
// (through the MIB_SERIAL_SYS_WRITE seam, to inject an EINTR stream) and
// tcflush (recorded, then passed through). Everything else is real, on a
// pseudo-terminal: the endless-EAGAIN case stops the pty's output with
// tcflow(TCOOFF), which makes the kernel refuse every write, as a stalled
// USB-serial adapter does.
// Standalone executable: it defines the platform-port symbols itself.

#include "backend/services/ISerialPort.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {
// TIOCOUTQ script: < 0 = the real ioctl; otherwise the queued byte count to
// report. outqError != 0 makes the call fail with that errno.
std::atomic<int> outqReport{-1};
std::atomic<int> outqError{0};
std::atomic<int> outqCalls{0};
std::atomic<int> outputFlushes{0}; // tcflush(TCOFLUSH): untransmitted output discarded
std::atomic<bool> writeEintr{false}; // every write(2) fails with EINTR
std::atomic<int> writeCalls{0};

ssize_t fakeWrite(int fd, const void* buf, size_t n)
{
    ++writeCalls;
    if (writeEintr.load()) {
        errno = EINTR;
        return -1;
    }
    return ::write(fd, buf, n);
}

int fakeIoctl(int fd, unsigned long request, int* arg)
{
    if (request == static_cast<unsigned long>(TIOCOUTQ)) {
        ++outqCalls;
        if (outqError.load() != 0) {
            errno = outqError.load();
            return -1;
        }
        if (outqReport.load() >= 0) {
            *arg = outqReport.load();
            return 0;
        }
    }
    return ::ioctl(fd, request, arg);
}

int fakeTcflush(int fd, int queue)
{
    if (queue == TCOFLUSH) ++outputFlushes;
    return ::tcflush(fd, queue);
}
} // namespace

#define ioctl fakeIoctl
#define tcflush fakeTcflush
#define MIB_SERIAL_SYS_WRITE fakeWrite
#include "../../src/backend/services/SerialPortPosix.cpp"
#undef ioctl
#undef tcflush

namespace {
using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::vector<uint8_t> readMaster(int fd, size_t want, int budgetMs)
{
    std::vector<uint8_t> out;
    const auto deadline = Clock::now() + std::chrono::milliseconds(budgetMs);
    while (out.size() < want && Clock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 20) > 0) {
            uint8_t buf[256];
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) out.insert(out.end(), buf, buf + n);
        }
    }
    return out;
}
} // namespace

int main()
{
    // Generous: every bound below is about a second; only a hang trips this.
    mib::test::Watchdog watchdog(20);
    // The ceiling on top of each bound: loaded CI and sanitizers may be slow,
    // a hang is not.
    constexpr long long kCeilingMs = 5000;

    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    MIB_REQUIRE(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0, "pty available");
    const std::string slave = ::ptsname(master);
    auto port = backend::services::makeSerialPortForPathForTesting(slave, 9600);
    MIB_REQUIRE(port != nullptr, "real POSIX port opens the pty slave");
    const std::vector<uint8_t> frame{0x01, 0x06, 0x00, 0x01, 0x00, 0x00, 0xD8, 0x0A};

    // 1) Happy path unchanged: the bytes go out, the drain is confirmed.
    watchdog.mark("happy path");
    MIB_EXPECT(port->write(frame) == static_cast<int>(frame.size()), "write takes the whole frame");
    MIB_EXPECT(port->waitForBytesWritten(1000), "drain confirmed (real TIOCOUTQ on a pty is 0)");
    MIB_EXPECT(readMaster(master, frame.size(), 2000) == frame, "the same bytes reach the wire");
    MIB_EXPECT(outputFlushes.load() == 0, "nothing discarded on the happy path");

    // 2) Output that never drains: tcdrain() would block for ever; the port
    //    reports a timeout at its bound and discards the queued bytes.
    watchdog.mark("output never drains");
    outqReport = 10;
    auto start = Clock::now();
    const bool drained = port->waitForBytesWritten(50);
    long long elapsed = msSince(start);
    std::printf("never-drains: returned after %lld ms (bound 50 ms)\n", elapsed);
    MIB_EXPECT(!drained, "a queue that never drains is reported, not waited out");
    MIB_EXPECT(elapsed >= 45 && elapsed < 50 + kCeilingMs, "returns at its timeout");
    MIB_EXPECT(port->lastError().find("timed out") != std::string::npos, "the error names the timeout");
    MIB_EXPECT(port->lastSystemError() == ETIMEDOUT, "ETIMEDOUT");
    MIB_EXPECT(outputFlushes.load() == 1, "the untransmitted output is discarded, not sent late");

    // 3) A queue that drains after a few polls is still confirmed.
    watchdog.mark("output drains late");
    outqReport = 3; // before the drainer starts, so its 0 cannot be overwritten
    std::thread drainer([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        outqReport = 0;
    });
    MIB_EXPECT(port->waitForBytesWritten(5000), "a slow drain within the timeout succeeds");
    drainer.join();
    MIB_EXPECT(outputFlushes.load() == 1, "a successful drain discards nothing");

    // 4) The queue cannot be read (driver error): fail at once, never block.
    watchdog.mark("TIOCOUTQ fails");
    outqError = EIO;
    start = Clock::now();
    MIB_EXPECT(!port->waitForBytesWritten(5000), "an unreadable queue fails");
    MIB_EXPECT(msSince(start) < kCeilingMs, "without waiting out the timeout");
    MIB_EXPECT(port->lastSystemError() == EIO, "the driver error is kept");
    outqError = 0;

    // 4b) An endless EINTR stream from the queue query: every retry is
    //     checked against the deadline, so it still times out.
    watchdog.mark("TIOCOUTQ EINTR stream");
    outqError = EINTR;
    const int flushesBeforeEintr = outputFlushes.load();
    const int queriesBefore = outqCalls.load();
    start = Clock::now();
    MIB_EXPECT(!port->waitForBytesWritten(50), "a drain interrupted for ever is not confirmed");
    elapsed = msSince(start);
    std::printf("TIOCOUTQ EINTR stream: returned after %lld ms (bound 50 ms, %d queries)\n", elapsed,
                outqCalls.load() - queriesBefore);
    MIB_EXPECT(elapsed >= 45 && elapsed < 50 + kCeilingMs, "returns at its timeout");
    MIB_EXPECT(port->lastError().find("timed out") != std::string::npos, "the error names the timeout");
    MIB_EXPECT(outputFlushes.load() == flushesBeforeEintr + 1, "and the output is discarded");
    outqError = 0;
    outqReport = -1;

    // 4c) An endless EINTR stream from write(2): same budget as EAGAIN.
    watchdog.mark("write EINTR stream");
    writeEintr = true;
    const int writesBefore = writeCalls.load();
    start = Clock::now();
    const int eintrWritten = port->write(frame);
    elapsed = msSince(start);
    writeEintr = false;
    std::printf("write EINTR stream: returned %d after %lld ms (bound %d ms, %d attempts)\n", eintrWritten,
                elapsed, backend::services::kSerialWriteTimeoutMs, writeCalls.load() - writesBefore);
    MIB_EXPECT(eintrWritten == -1, "a write interrupted for ever fails");
    MIB_EXPECT(elapsed >= backend::services::kSerialWriteTimeoutMs - 50 &&
                   elapsed < backend::services::kSerialWriteTimeoutMs + kCeilingMs,
               "at the write timeout, not for ever");
    MIB_EXPECT(port->lastError().find("timed out") != std::string::npos, "the error names the timeout");

    // 5) Endless EAGAIN: output stopped on the tty, every write refused. The
    //    old loop retried for ever; now write() waits for room with poll()
    //    up to kSerialWriteTimeoutMs and fails.
    watchdog.mark("endless EAGAIN");
    const int control = ::open(slave.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    MIB_REQUIRE(control >= 0, "second handle on the pty slave");
    MIB_REQUIRE(::tcflow(control, TCOOFF) == 0, "pty output stopped");
    start = Clock::now();
    const int written = port->write(frame);
    elapsed = msSince(start);
    std::printf("endless-EAGAIN: write returned %d after %lld ms (bound %d ms)\n", written, elapsed,
                backend::services::kSerialWriteTimeoutMs);
    MIB_EXPECT(written == -1, "a write the driver never takes fails");
    MIB_EXPECT(elapsed >= backend::services::kSerialWriteTimeoutMs - 50 &&
                   elapsed < backend::services::kSerialWriteTimeoutMs + kCeilingMs,
               "after waiting for room up to the write timeout, not at once and not for ever");
    MIB_EXPECT(port->lastError().find("timed out") != std::string::npos, "the error names the timeout");
    MIB_EXPECT(port->lastSystemError() == ETIMEDOUT, "ETIMEDOUT");
    MIB_EXPECT(outputFlushes.load() == 4, "a partial frame is discarded, not sent late");

    // 6) The adapter recovers: the port works again with no reopen.
    watchdog.mark("recovery");
    MIB_REQUIRE(::tcflow(control, TCOON) == 0, "pty output restarted");
    (void)readMaster(master, 1, 100); // nothing stale may arrive; drop it if it does
    MIB_EXPECT(port->write(frame) == static_cast<int>(frame.size()), "write works after the stall");
    MIB_EXPECT(port->waitForBytesWritten(1000), "and drains");
    MIB_EXPECT(readMaster(master, frame.size(), 2000) == frame, "the new frame arrives intact");

    // 7) interrupt() from another thread (bus session teardown): a write
    //    waiting for room wakes at once and fails; later waits fail too.
    watchdog.mark("interrupt");
    MIB_REQUIRE(::tcflow(control, TCOOFF) == 0, "pty output stopped again");
    std::atomic<bool> writing{false};
    int interruptedWrite = 0;
    std::string interruptedError;
    std::thread writer([&] {
        writing = true;
        interruptedWrite = port->write(frame);
        interruptedError = port->lastError();
    });
    while (!writing.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // let it reach its poll()
    port->interrupt();
    writer.join();
    std::printf("interrupt: write returned %d: %s\n", interruptedWrite, interruptedError.c_str());
    MIB_EXPECT(interruptedWrite == -1, "the interrupted write fails");
    MIB_EXPECT(interruptedError.find("interrupted") != std::string::npos,
               "woken by the interrupt, not by its write timeout");
    start = Clock::now();
    MIB_EXPECT(!port->waitForReadyRead(5000), "a wait after the interrupt fails");
    MIB_EXPECT(!port->waitForBytesWritten(5000), "so does a drain");
    MIB_EXPECT(msSince(start) < kCeilingMs, "at once");
    MIB_REQUIRE(::tcflow(control, TCOON) == 0, "pty output restarted");

    port->close();
    MIB_EXPECT(!port->waitForBytesWritten(0), "closed port fails");
    MIB_EXPECT(port->write(frame) == -1, "closed port refuses writes");
    ::close(control);
    ::close(master);
    if (mib::test::exitCode() == 0) std::printf("POSIX serial deadlines verified\n");
    return mib::test::exitCode();
}
