#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#endif

namespace backend::services::serialbus {

namespace {
    // Modbus RTU inter-frame silence (3.5 char times: ~4 ms at 9600 baud),
    // measured from the last bus activity — an idle bus needs no extra wait.
    constexpr auto INTER_FRAME_DELAY = std::chrono::milliseconds(5);
    // After a complete valid frame, listen briefly for trailing bytes — a
    // second device answering the same address shows up here.
    constexpr int COLLISION_LISTEN_MS = 5;
    // transact() gives the I/O thread the transaction's own worst case plus
    // this much for scheduling delays (loaded hosts, sanitizer builds), #720.
    constexpr int TRANSACT_SLACK_MS = 500;

    std::string hex(const Bytes& b)
    {
        std::string out;
        char buf[4];
        for (size_t i = 0; i < b.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%02x", b[i]);
            if (i) out += ' ';
            out += buf;
        }
        return out;
    }

    // Classify an open() failure from the OS error code, never from text.
    BusError classifyOpenError(int systemError)
    {
#if defined(_WIN32)
        if (systemError == ERROR_ACCESS_DENIED || systemError == ERROR_SHARING_VIOLATION) {
            return BusError::PortBusy; // held by another process, or no rights
        }
#else
        if (systemError == EBUSY || systemError == EACCES || systemError == EPERM) {
            return BusError::PortBusy;
        }
#endif
        return BusError::PortUnavailable;
    }
} // namespace

std::vector<PortInfo> availablePorts()
{
    return enumerateSerialPorts();
}

const char* toString(BusError error)
{
    switch (error) {
    case BusError::None:               return "ok";
    case BusError::PortUnavailable:    return "port unavailable";
    case BusError::PortBusy:           return "port busy";
    case BusError::NotOpen:            return "port not open";
    case BusError::WriteFailed:        return "write failed";
    case BusError::Timeout:            return "bus timeout";
    case BusError::CrcError:           return "CRC error (possible address collision)";
    case BusError::FrameError:         return "frame error";
    case BusError::WrongAddress:       return "response from wrong address";
    case BusError::WrongFunction:      return "response with wrong function code";
    case BusError::ModbusException:    return "Modbus exception";
    case BusError::CollisionSuspected: return "duplicate-address collision suspected";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// ModbusBusSession
// ---------------------------------------------------------------------------

// Shared by the session and its I/O thread (the thread's lambda holds a
// shared_ptr), so a thread detached at teardown never touches freed memory.
struct ModbusBusSession::IoState {
    IoState(std::string name, const SerialSettings& lineSettings, SerialPortFactory portFactory)
        : portName(std::move(name)), settings(lineSettings), factory(std::move(portFactory))
    {
    }

    void run();                                               // the I/O thread
    bool openPort();                                          // I/O thread only
    Transaction runTransaction(const Bytes& request, int timeoutMs); // I/O thread only

    const std::string portName;
    const SerialSettings settings;
    const SerialPortFactory factory;

    // Used by the I/O thread only. `serial` is also assigned and released
    // under `mutex`, so the destructor can interrupt it from another thread.
    std::unique_ptr<ISerialPort> serial;
    std::chrono::steady_clock::time_point lastBusActivity{};

    // Handshake between callers and the I/O thread, all under `mutex`.
    // Sequence numbers start at 1; 0 means "none".
    std::mutex mutex;
    std::condition_variable cv;
    Bytes jobRequest;              // a copy: a caller that timed out may be gone
    int jobTimeoutMs{0};
    std::uint64_t lastSeq{0};      // last sequence number handed out
    std::uint64_t pendingSeq{0};   // posted, not yet taken by the I/O thread
    std::uint64_t runningSeq{0};   // being run by the I/O thread
    std::uint64_t abandonedSeq{0}; // its caller gave up; its result is dropped
    std::uint64_t doneSeq{0};      // the request jobResult answers
    Transaction jobResult;
    // Written under `mutex`; also read by the I/O thread mid-transaction.
    std::atomic<bool> stopRequested{false};
    bool openDone{false};
    bool exited{false};            // the I/O thread has closed the port and is leaving
    BusError openErrorCode{BusError::PortUnavailable};
    std::string openErrorDetail;
    std::atomic<bool> portOpen{false};
};

ModbusBusSession::ModbusBusSession(std::string portName, const SerialSettings& settings,
                                   SerialPortFactory factory)
    : portName_(std::move(portName)), settings_(settings),
      io_(std::make_shared<IoState>(portName_, settings_, std::move(factory)))
{
}

ModbusBusSession::~ModbusBusSession()
{
    if (!ioThread_.joinable()) {
        // start() failed and already joined the thread.
        SPDLOG_INFO("SerialBus: session on {} closed", portName_);
        return;
    }
    {
        std::scoped_lock lock(io_->mutex);
        io_->stopRequested.store(true);
        // A transaction still running belongs to a caller that gave up (or
        // there would be no destructor call): wake its port call so the I/O
        // thread can close the port. The thread itself closes it.
        if (io_->runningSeq != 0 && io_->serial) {
            io_->serial->interrupt();
        }
    }
    io_->cv.notify_all();

    bool exited = false;
    {
        std::unique_lock lock(io_->mutex);
        exited = io_->cv.wait_for(lock, std::chrono::milliseconds(kShutdownJoinMs),
                                  [this] { return io_->exited; });
    }
    if (exited) {
        ioThread_.join(); // it is past its last use of the state
        SPDLOG_INFO("SerialBus: session on {} closed", portName_);
        return;
    }
    // Still inside a port call (stuck in the driver). It holds its own
    // reference to the state and closes the port when the call returns.
    ioThread_.detach();
    SPDLOG_ERROR("SerialBus: session on {} closed, but its I/O thread is still inside a port call "
                 "after {} ms: detached; it closes the port when the call returns",
                 portName_, kShutdownJoinMs);
}

bool ModbusBusSession::isOpen() const
{
    return io_->portOpen.load();
}

bool ModbusBusSession::start(BusError* error, std::string* errorDetail)
{
    ioThread_ = std::thread([io = io_] { io->run(); });
    std::unique_lock lock(io_->mutex);
    io_->cv.wait(lock, [this] { return io_->openDone; });
    if (!io_->portOpen.load()) {
        if (error) *error = io_->openErrorCode;
        if (errorDetail) *errorDetail = io_->openErrorDetail;
        lock.unlock();
        ioThread_.join();
        return false;
    }
    return true;
}

bool ModbusBusSession::IoState::openPort()
{
    auto port = factory ? factory() : makePlatformSerialPort();
    if (!port) {
        std::scoped_lock lock(mutex);
        openErrorCode = BusError::PortUnavailable;
        openErrorDetail = "no serial port implementation";
        return false;
    }
    if (!port->openNamed(portName, settings)) {
        SPDLOG_ERROR("SerialBus: failed to open {}: {}", portName, port->lastError());
        std::scoped_lock lock(mutex);
        openErrorCode = classifyOpenError(port->lastSystemError());
        openErrorDetail = port->lastError();
        return false;
    }
    SPDLOG_INFO("SerialBus: {} opened ({} {}{}{})", portName, settings.baudRate,
                settings.dataBits, settings.parity, settings.stopBits);
    std::scoped_lock lock(mutex);
    serial = std::move(port);
    return true;
}

void ModbusBusSession::IoState::run()
{
    // The port is created, used, and closed only on this thread.
    const bool opened = openPort();
    portOpen.store(opened);
    {
        std::scoped_lock lock(mutex);
        openDone = true;
    }
    cv.notify_all();

    if (opened) {
        lastBusActivity = std::chrono::steady_clock::now() - INTER_FRAME_DELAY;
        while (true) {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return pendingSeq != 0 || stopRequested.load(); });
            if (stopRequested.load()) {
                break;
            }
            const std::uint64_t seq = pendingSeq;
            const Bytes request = std::move(jobRequest);
            jobRequest.clear();
            const int timeoutMs = jobTimeoutMs;
            pendingSeq = 0;
            runningSeq = seq;
            lock.unlock();

            Transaction result = runTransaction(request, timeoutMs);

            lock.lock();
            runningSeq = 0;
            if (abandonedSeq == seq) {
                // Its caller already returned Timeout. The result answers
                // nobody: drop it, so it can never be taken for a later request.
                abandonedSeq = 0;
                lock.unlock();
                SPDLOG_WARN("SerialBus: {} dropped the late result ({}) of a transaction its caller "
                            "gave up on", portName, toString(result.error));
                continue;
            }
            doneSeq = seq;
            jobResult = std::move(result);
            lock.unlock();
            cv.notify_all();
        }
    }

    // Close the port here, on its own thread, then tell the destructor (if it
    // is still waiting) that nothing else will touch the port.
    std::unique_ptr<ISerialPort> port;
    {
        std::scoped_lock lock(mutex);
        port = std::move(serial);
    }
    portOpen.store(false);
    if (port) {
        if (port->isOpen()) {
            port->close();
        }
        port.reset();
    }
    {
        std::scoped_lock lock(mutex);
        exited = true;
    }
    cv.notify_all();
}

int ModbusBusSession::transactBudgetMs(int timeoutMs)
{
    const long long t = std::max(0, timeoutMs);
    const long long budget = static_cast<long long>(INTER_FRAME_DELAY.count()) +
                             kSerialWriteTimeoutMs + t /* drain */ + t /* response */ +
                             COLLISION_LISTEN_MS + TRANSACT_SLACK_MS;
    return static_cast<int>(std::min<long long>(budget, INT_MAX));
}

Transaction ModbusBusSession::transact(const Bytes& request, int timeoutMs)
{
    std::scoped_lock callLock(callMutex_);
    Transaction result;
    if (!io_->portOpen.load()) {
        result.error = BusError::NotOpen;
        return result;
    }
    const int budgetMs = transactBudgetMs(timeoutMs);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);

    IoState& io = *io_;
    std::unique_lock lock(io.mutex);
    if (io.runningSeq != 0) {
        // Callers are serialized, so the I/O thread is only busy here with a
        // request whose caller gave up: its port call is stuck. Fail fast
        // rather than queue behind it.
        result.error = BusError::Timeout;
        result.detail = "the serial I/O thread is still stuck in an earlier transaction";
        lock.unlock();
        SPDLOG_ERROR("SerialBus: {} {}; request not sent", portName_, result.detail);
        return result;
    }
    const std::uint64_t seq = ++io.lastSeq;
    io.jobRequest = request;
    io.jobTimeoutMs = timeoutMs;
    io.pendingSeq = seq;
    io.cv.notify_all();
    if (io.cv.wait_until(lock, deadline, [&io, seq] { return io.doneSeq == seq; })) {
        return std::move(io.jobResult);
    }

    // No result in time (#720). Whatever the I/O thread does next cannot
    // answer this caller: a request it never took is withdrawn (never sent),
    // a request it is running is marked abandoned (its result is dropped).
    result.error = BusError::Timeout;
    if (io.pendingSeq == seq) {
        io.pendingSeq = 0;
        io.jobRequest.clear();
        result.detail = "the serial I/O thread did not take the request within " +
                        std::to_string(budgetMs) + " ms (withdrawn, not sent)";
    } else {
        io.abandonedSeq = seq;
        result.detail = "no result from the serial I/O thread within " + std::to_string(budgetMs) +
                        " ms (a port call is stuck)";
    }
    lock.unlock();
    SPDLOG_ERROR("SerialBus: {} transaction timed out: {}", portName_, result.detail);
    return result;
}

Transaction ModbusBusSession::IoState::runTransaction(const Bytes& request, int timeoutMs)
{
    Transaction result;

    if (!serial || !serial->isOpen()) {
        result.error = BusError::NotOpen;
        return result;
    }
    if (request.size() < 2) {
        result.error = BusError::WriteFailed;
        return result;
    }

    // Drain stale bytes (e.g. a response that arrived after a previous
    // transaction's deadline) so they cannot be attributed to this request.
    const Bytes stale = serial->readAll();
    if (!stale.empty()) {
        SPDLOG_DEBUG("SerialBus: {} discarded {} stale bytes: {}", portName, stale.size(), hex(stale));
    }

    // RTU inter-frame silence, measured from the last bus activity so an
    // already-idle bus pays nothing.
    const auto sinceLast = std::chrono::steady_clock::now() - lastBusActivity;
    if (sinceLast < INTER_FRAME_DELAY) {
        std::this_thread::sleep_for(INTER_FRAME_DELAY - sinceLast);
    }

    if (stopRequested.load()) {
        result.error = BusError::NotOpen;
        result.detail = "session closing";
        return result;
    }
    SPDLOG_DEBUG("SerialBus: {} TX [{}]: {}", portName, request.size(), hex(request));

    // Both calls are bounded by the port (#720): write() by
    // kSerialWriteTimeoutMs, the drain by timeoutMs. On a timeout the port
    // has already discarded the untransmitted bytes.
    if (serial->write(request) != static_cast<int>(request.size())) {
        result.detail = serial->lastError();
        if (result.detail.empty()) result.detail = "serial write failed";
        SPDLOG_ERROR("SerialBus: {} failed to write {} bytes: {}", portName, request.size(), result.detail);
        result.error = BusError::WriteFailed;
        return result;
    }
    if (!serial->waitForBytesWritten(timeoutMs)) {
        result.detail = serial->lastError();
        if (result.detail.empty()) {
            result.detail = "serial output did not drain within " + std::to_string(timeoutMs) + " ms";
        }
        SPDLOG_ERROR("SerialBus: {} write timeout: {}", portName, result.detail);
        result.error = BusError::WriteFailed;
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    Bytes buffer;
    // When a complete frame from another slave address is discarded, remember
    // it so a final timeout is reported as WrongAddress, not a silent Timeout.
    bool sawWrongAddress = false;

    const auto finish = [&](BusError error) {
        lastBusActivity = std::chrono::steady_clock::now();
        result.error = error;
        return result;
    };
    const auto append = [](Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); };

    while (true) {
        if (stopRequested.load()) {
            // The session is closing (its port was interrupted): stop reading.
            result.detail = "session closing";
            return finish(BusError::Timeout);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            SPDLOG_ERROR("SerialBus: {} timeout ({} bytes buffered): {}", portName, buffer.size(), hex(buffer));
            result.detail = (sawWrongAddress ? "only other addresses answered within "
                                             : "no response within ") +
                            std::to_string(timeoutMs) + " ms";
            return finish(sawWrongAddress ? BusError::WrongAddress : BusError::Timeout);
        }
        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int expected = modbus::expectedFrameLength(buffer);
        if (buffer.empty() || expected < 0 || static_cast<int>(buffer.size()) < expected) {
            if (!serial->waitForReadyRead(std::max(1, remainingMs))) {
                continue; // deadline check at loop top decides
            }
            append(buffer, serial->readAll());
        }

        const int frameLen = modbus::expectedFrameLength(buffer);
        if (frameLen == -1) {
            continue; // need more header bytes
        }
        if (frameLen == -2) {
            SPDLOG_ERROR("SerialBus: {} unframeable bytes: {}", portName, hex(buffer));
            return finish(BusError::FrameError);
        }
        if (static_cast<int>(buffer.size()) < frameLen) {
            continue; // frame incomplete
        }

        const Bytes frame(buffer.begin(), buffer.begin() + frameLen);
        const auto verdict = modbus::classifyResponse(request, frame);
        if (verdict == modbus::ResponseVerdict::WrongAddress) {
            // Stale/delayed frame from another device: discard, keep reading.
            SPDLOG_WARN("SerialBus: {} discarding frame from addr {} while waiting on addr {}",
                        portName, frame[0], request[0]);
            buffer.erase(buffer.begin(), buffer.begin() + frameLen);
            sawWrongAddress = true;
            continue;
        }

        SPDLOG_DEBUG("SerialBus: {} RX [{}]: {}", portName, frame.size(), hex(frame));

        switch (verdict) {
        case modbus::ResponseVerdict::Ok:
        case modbus::ResponseVerdict::Exception:
            break;
        case modbus::ResponseVerdict::CrcMismatch:
            SPDLOG_ERROR("SerialBus: {} CRC mismatch (possible duplicate-address collision): {}",
                         portName, hex(frame));
            return finish(BusError::CrcError);
        case modbus::ResponseVerdict::WrongFunction:
            SPDLOG_ERROR("SerialBus: {} wrong function code in response: {}", portName, hex(frame));
            return finish(BusError::WrongFunction);
        default:
            SPDLOG_ERROR("SerialBus: {} malformed response: {}", portName, hex(frame));
            return finish(BusError::FrameError);
        }

        // Valid frame. Anything already buffered past it, or arriving in the
        // collision-listen window, means a second device answered too.
        buffer.erase(buffer.begin(), buffer.begin() + frameLen);
        if (buffer.empty() && serial->waitForReadyRead(COLLISION_LISTEN_MS)) {
            append(buffer, serial->readAll());
        }
        if (!buffer.empty()) {
            SPDLOG_ERROR("SerialBus: {} {} trailing bytes after a valid frame — "
                         "duplicate-address collision suspected: {}",
                         portName, buffer.size(), hex(buffer));
            return finish(BusError::CollisionSuspected);
        }

        if (verdict == modbus::ResponseVerdict::Exception) {
            result.exceptionCode = frame[2];
            SPDLOG_ERROR("SerialBus: {} Modbus exception 0x{:02X} (func=0x{:02X}, addr={})",
                         portName, result.exceptionCode, frame[1] & 0x7F, frame[0]);
            result.response = frame;
            return finish(BusError::ModbusException);
        }
        result.response = frame;
        return finish(BusError::None);
    }
}

// ---------------------------------------------------------------------------
// SerialBusManager
// ---------------------------------------------------------------------------
SerialBusManager::SerialBusManager() : factory_([] { return makePlatformSerialPort(); }) {}

void SerialBusManager::setSerialPortFactory(SerialPortFactory factory)
{
    std::scoped_lock lock(mutex_);
    factory_ = std::move(factory);
}

std::string SerialBusManager::normalizeKey(const std::string& portName)
{
    // "COM3", "\\.\COM3", "ttyUSB0" and "/dev/ttyUSB0" must map to one bus.
    std::string key = portName;
    key.erase(0, key.find_first_not_of(" \t"));
    if (const auto end = key.find_last_not_of(" \t"); end != std::string::npos) key.erase(end + 1);
    if (key.rfind("\\\\.\\", 0) == 0) key.erase(0, 4);
    if (const auto slash = key.find_last_of('/'); slash != std::string::npos) key = key.substr(slash + 1);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

std::shared_ptr<ModbusBusSession> SerialBusManager::acquire(const std::string& portName,
                                                            const SerialSettings& settings,
                                                            BusError* error,
                                                            std::string* errorDetail)
{
    const std::string key = normalizeKey(portName);
    std::scoped_lock lock(mutex_);

    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        if (auto existing = it->second.lock()) {
            if (existing->settings() != settings) {
                SPDLOG_ERROR("SerialBus: {} already open with different settings "
                             "({} vs requested {})", portName,
                             existing->settings().baudRate, settings.baudRate);
                if (error) *error = BusError::PortBusy;
                if (errorDetail) *errorDetail = "port already in use with different serial settings";
                return nullptr;
            }
            if (error) *error = BusError::None;
            return existing;
        }
        sessions_.erase(it);
    }

    auto* raw = new ModbusBusSession(portName, settings, factory_);
    BusError openError = BusError::PortUnavailable;
    std::string detail;
    if (!raw->start(&openError, &detail)) {
        delete raw;
        if (error) *error = openError;
        if (errorDetail) *errorDetail = detail;
        return nullptr;
    }
    // The deleter closes the port and erases the registry entry atomically
    // under the registry lock, so a dying session and a fresh acquire of the
    // same port cannot interleave.
    std::shared_ptr<ModbusBusSession> session(raw, [this, key](ModbusBusSession* s) {
        std::scoped_lock l(mutex_);
        auto entry = sessions_.find(key);
        if (entry != sessions_.end() && entry->second.expired()) {
            sessions_.erase(entry);
        }
        delete s;
    });
    sessions_[key] = session;
    if (error) *error = BusError::None;
    return session;
}

} // namespace backend::services::serialbus
