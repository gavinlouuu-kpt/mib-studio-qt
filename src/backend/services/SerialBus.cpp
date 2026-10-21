#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"

#include <spdlog/sinks/null_sink.h>
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
    // acquire() waits this much past the destructor's bound (kShutdownJoinMs)
    // for a closing session to decide (closed or stalled) before refusing anyway.
    constexpr int CLOSING_SLACK_MS = 250;

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

// Created with the session and held by its I/O thread until the thread has
// closed the port; SerialBusManager keeps only a weak reference. A session
// that is gone (its last shared_ptr dropped) while its reservation lives is
// still closing its port: its destructor is waiting for the I/O thread
// (bounded) or has detached it after a stalled transaction (`stalled`).
struct ModbusBusSession::PortReservation {
    enum class State { Closing, Stalled, Released };

    State state()
    {
        std::scoped_lock lock(mutex);
        return stateLocked();
    }

    // Waits until the port is released or known to be stalled, or until
    // `deadline`. Callers hold no other lock.
    State waitUntil(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex);
        cv.wait_until(lock, deadline, [this] { return released || stalled; });
        return stateLocked();
    }

    void release() { set(released); } // the I/O thread closed the port
    void markStalled() { set(stalled); } // the destructor detached the I/O thread

private:
    State stateLocked() const
    {
        return released ? State::Released : stalled ? State::Stalled : State::Closing;
    }
    void set(bool& flag)
    {
        {
            std::scoped_lock lock(mutex);
            flag = true;
        }
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool released{false};
    bool stalled{false};
};

// Shared by the session and its I/O thread (the thread's lambda holds a
// shared_ptr), so a thread detached at teardown never touches freed memory.
struct ModbusBusSession::IoState {
    IoState(std::string name, const SerialSettings& lineSettings, SerialPortFactory portFactory)
        : portName(std::move(name)), settings(lineSettings), factory(std::move(portFactory)),
          logger(sessionLogger())
    {
    }

    // The session logs through its own reference to the logger it was created
    // with, never through spdlog's global default: a detached I/O thread can
    // still log after the application has dropped or replaced its loggers
    // (spdlog::drop_all / shutdown at exit), and the global macros would then
    // dereference a null default logger.
    static std::shared_ptr<spdlog::logger> sessionLogger()
    {
        if (auto current = spdlog::default_logger()) return current;
        return std::make_shared<spdlog::logger>("serialbus",
                                                std::make_shared<spdlog::sinks::null_sink_mt>());
    }

    void run();                                               // the I/O thread
    bool openPort();                                          // I/O thread only
    Transaction runTransaction(const Bytes& request, int timeoutMs); // I/O thread only

    const std::string portName;
    const SerialSettings settings;
    const SerialPortFactory factory;
    const std::shared_ptr<spdlog::logger> logger; // every log line of the session goes here

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
    // Released by the I/O thread right after it closed the port (see
    // SerialBusManager::acquire).
    std::shared_ptr<PortReservation> reservation{std::make_shared<PortReservation>()};
    // Keeps this process's discovery probes off the port until it is closed
    // (SerialPortClaims.h); released with the reservation.
    portclaims::Claim claim;
    BusError openErrorCode{BusError::PortUnavailable};
    std::string openErrorDetail;
    std::atomic<bool> portOpen{false};
};

ModbusBusSession::ModbusBusSession(std::string portName, const SerialSettings& settings,
                                   SerialPortFactory factory, int shutdownJoinMs)
    : portName_(std::move(portName)), settings_(settings), shutdownJoinMs_(shutdownJoinMs),
      io_(std::make_shared<IoState>(portName_, settings_, std::move(factory)))
{
}

ModbusBusSession::~ModbusBusSession()
{
    const auto& log = io_->logger;
    if (!ioThread_.joinable()) {
        // start() failed and already joined the thread.
        SPDLOG_LOGGER_INFO(log, "SerialBus: session on {} closed", portName_);
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
        exited = io_->cv.wait_for(lock, std::chrono::milliseconds(shutdownJoinMs_),
                                  [this] { return io_->exited; });
        if (!exited && io_->reservation) {
            io_->reservation->markStalled(); // acquire() refuses the port, and says why
        }
    }
    if (exited) {
        ioThread_.join(); // it is past its last use of the state
        SPDLOG_LOGGER_INFO(log, "SerialBus: session on {} closed", portName_);
        return;
    }
    // Still inside a port call (stuck in the driver). It holds its own
    // reference to the state and closes the port when the call returns; the
    // port stays reserved until then.
    ioThread_.detach();
    SPDLOG_LOGGER_ERROR(log, "SerialBus: session on {} closed, but its I/O thread is still inside "
                             "a port call after {} ms: detached; the port stays reserved until the "
                             "thread closes it", portName_, shutdownJoinMs_);
}

std::weak_ptr<ModbusBusSession::PortReservation> ModbusBusSession::reservation() const
{
    std::scoped_lock lock(io_->mutex);
    return io_->reservation;
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
        SPDLOG_LOGGER_ERROR(logger, "SerialBus: failed to open {}: {}", portName, port->lastError());
        std::scoped_lock lock(mutex);
        openErrorCode = classifyOpenError(port->lastSystemError());
        openErrorDetail = port->lastError();
        return false;
    }
    SPDLOG_LOGGER_INFO(logger, "SerialBus: {} opened ({} {}{}{})", portName, settings.baudRate,
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
                SPDLOG_LOGGER_WARN(logger, "SerialBus: {} dropped the late result ({}) of a transaction "
                                   "its caller gave up on", portName, toString(result.error));
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
        reservation->release(); // the port is closed: a new session may open it
        reservation.reset();
        claim.release();
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
        SPDLOG_LOGGER_ERROR(io.logger, "SerialBus: {} {}; request not sent", portName_,
                            result.detail);
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
    SPDLOG_LOGGER_ERROR(io.logger, "SerialBus: {} transaction timed out: {}", portName_,
                        result.detail);
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
        SPDLOG_LOGGER_DEBUG(logger, "SerialBus: {} discarded {} stale bytes: {}", portName,
                            stale.size(), hex(stale));
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
    SPDLOG_LOGGER_DEBUG(logger, "SerialBus: {} TX [{}]: {}", portName, request.size(), hex(request));

    // Both calls are bounded by the port (#720): write() by
    // kSerialWriteTimeoutMs, the drain by timeoutMs. On a timeout the port
    // has already discarded the untransmitted bytes.
    if (serial->write(request) != static_cast<int>(request.size())) {
        result.detail = serial->lastError();
        if (result.detail.empty()) result.detail = "serial write failed";
        SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} failed to write {} bytes: {}", portName,
                            request.size(), result.detail);
        result.error = BusError::WriteFailed;
        return result;
    }
    if (!serial->waitForBytesWritten(timeoutMs)) {
        result.detail = serial->lastError();
        if (result.detail.empty()) {
            result.detail = "serial output did not drain within " + std::to_string(timeoutMs) + " ms";
        }
        SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} write timeout: {}", portName, result.detail);
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
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} timeout ({} bytes buffered): {}", portName,
                                buffer.size(), hex(buffer));
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
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} unframeable bytes: {}", portName, hex(buffer));
            return finish(BusError::FrameError);
        }
        if (static_cast<int>(buffer.size()) < frameLen) {
            continue; // frame incomplete
        }

        const Bytes frame(buffer.begin(), buffer.begin() + frameLen);
        const auto verdict = modbus::classifyResponse(request, frame);
        if (verdict == modbus::ResponseVerdict::WrongAddress) {
            // Stale/delayed frame from another device: discard, keep reading.
            SPDLOG_LOGGER_WARN(logger, "SerialBus: {} discarding frame from addr {} while waiting "
                               "on addr {}", portName, frame[0], request[0]);
            buffer.erase(buffer.begin(), buffer.begin() + frameLen);
            sawWrongAddress = true;
            continue;
        }

        SPDLOG_LOGGER_DEBUG(logger, "SerialBus: {} RX [{}]: {}", portName, frame.size(), hex(frame));

        switch (verdict) {
        case modbus::ResponseVerdict::Ok:
        case modbus::ResponseVerdict::Exception:
            break;
        case modbus::ResponseVerdict::CrcMismatch:
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} CRC mismatch (possible duplicate-address "
                                "collision): {}", portName, hex(frame));
            return finish(BusError::CrcError);
        case modbus::ResponseVerdict::WrongFunction:
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} wrong function code in response: {}",
                                portName, hex(frame));
            return finish(BusError::WrongFunction);
        default:
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} malformed response: {}", portName, hex(frame));
            return finish(BusError::FrameError);
        }

        // Valid frame. Anything already buffered past it, or arriving in the
        // collision-listen window, means a second device answered too.
        buffer.erase(buffer.begin(), buffer.begin() + frameLen);
        if (buffer.empty() && serial->waitForReadyRead(COLLISION_LISTEN_MS)) {
            append(buffer, serial->readAll());
        }
        if (!buffer.empty()) {
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} {} trailing bytes after a valid frame — "
                                "duplicate-address collision suspected: {}",
                                portName, buffer.size(), hex(buffer));
            return finish(BusError::CollisionSuspected);
        }

        if (verdict == modbus::ResponseVerdict::Exception) {
            result.exceptionCode = frame[2];
            SPDLOG_LOGGER_ERROR(logger, "SerialBus: {} Modbus exception 0x{:02X} (func=0x{:02X}, "
                                "addr={})", portName, result.exceptionCode, frame[1] & 0x7F, frame[0]);
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

void SerialBusManager::setShutdownJoinMsForTesting(int ms)
{
    std::scoped_lock lock(mutex_);
    shutdownJoinMs_ = std::max(0, ms);
}

std::string SerialBusManager::normalizeKey(const std::string& portName)
{
    // "COM3", "\\.\COM3", "ttyUSB0" and "/dev/ttyUSB0" must map to one bus,
    // and to the same key the port claims use.
    return portclaims::key(portName);
}

bool SerialBusManager::hasSession(const std::string& portName)
{
    std::scoped_lock lock(mutex_);
    const auto it = sessions_.find(normalizeKey(portName));
    return it != sessions_.end() && !it->second.session.expired();
}

std::shared_ptr<ModbusBusSession> SerialBusManager::acquire(const std::string& portName,
                                                            const SerialSettings& settings,
                                                            BusError* error,
                                                            std::string* errorDetail)
{
    using Clock = std::chrono::steady_clock;
    const std::string key = normalizeKey(portName);
    // A discovery probe of this process may be on the port right now: the
    // startup nanopositioner probe (CH34x adapters, the common RS485 dongle) or
    // the pulse generator's "auto" sweep (every USB adapter, any chip, at its
    // own baud). Wait for it, unlocked, rather than fail with "busy" or with
    // "different settings" against the probe's short-lived session
    // (SerialPortClaims.h). A probe on this thread is not waited for. The
    // claim is kept only if this call opens a new session.
    auto claim = portclaims::claimForService(portName, "serial bus " + portName, portclaims::kProbeYield,
                                             /*busSession=*/true);

    // A session that is gone may still be closing its port. Its destructor
    // decides within kShutdownJoinMs: closed (the reservation is released) or
    // stalled (the I/O thread is detached, still holding the port). Wait for
    // that answer, never longer and never under the registry lock.
    auto refuseClosing = [&](const char* why) -> std::shared_ptr<ModbusBusSession> {
        const std::string detail = "serial port " + portName + " is still closing" + why;
        SPDLOG_WARN("SerialBus: {}", detail);
        if (error) *error = BusError::PortBusy;
        if (errorDetail) *errorDetail = detail;
        return nullptr;
    };

    std::unique_lock lock(mutex_);
    const auto closingDeadline =
        Clock::now() + std::chrono::milliseconds(shutdownJoinMs_ + CLOSING_SLACK_MS);
    while (true) {
        auto it = sessions_.find(key);
        if (it == sessions_.end()) break;
        if (auto existing = it->second.session.lock()) {
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
        auto reservation = it->second.reservation.lock();
        const auto state =
            reservation ? reservation->state() : ModbusBusSession::PortReservation::State::Released;
        if (state == ModbusBusSession::PortReservation::State::Closing) {
            if (Clock::now() >= closingDeadline) return refuseClosing("");
            lock.unlock();
            ++closingWaiters_;
            reservation->waitUntil(closingDeadline);
            --closingWaiters_;
            reservation.reset();
            lock.lock();
            continue; // the registry may have changed meanwhile: look again
        }
        if (state == ModbusBusSession::PortReservation::State::Stalled) {
            return refuseClosing(" after a stalled transaction");
        }
        sessions_.erase(it); // closed: the port is free
        break;
    }

    auto* raw = new ModbusBusSession(portName, settings, factory_, shutdownJoinMs_);
    raw->io_->claim = std::move(claim); // released when its I/O thread closed the port
    BusError openError = BusError::PortUnavailable;
    std::string detail;
    if (!raw->start(&openError, &detail)) {
        delete raw; // its I/O thread has already closed the port and been joined
        if (error) *error = openError;
        if (errorDetail) *errorDetail = detail;
        return nullptr;
    }
    // Plain delete, without the registry lock: the destructor may wait up to
    // its bound (kShutdownJoinMs) for the I/O thread. The registry keeps the port
    // reserved through the session's reservation until the thread closed it.
    std::shared_ptr<ModbusBusSession> session(raw, [](ModbusBusSession* s) { delete s; });
    sessions_[key] = Entry{session, session->reservation()};
    if (error) *error = BusError::None;
    return session;
}

} // namespace backend::services::serialbus
