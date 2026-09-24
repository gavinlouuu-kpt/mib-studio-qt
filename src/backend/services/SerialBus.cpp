#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
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
ModbusBusSession::ModbusBusSession(std::string portName, const SerialSettings& settings,
                                   SerialPortFactory factory)
    : portName_(std::move(portName)), settings_(settings), factory_(std::move(factory))
{
}

ModbusBusSession::~ModbusBusSession()
{
    {
        std::scoped_lock lock(jobMutex_);
        stopRequested_ = true;
    }
    jobCv_.notify_all();
    if (ioThread_.joinable()) {
        ioThread_.join(); // the io thread closes the port before exiting
    }
    SPDLOG_INFO("SerialBus: session on {} closed", portName_);
}

bool ModbusBusSession::start(BusError* error, std::string* errorDetail)
{
    ioThread_ = std::thread([this] { ioLoop(); });
    std::unique_lock lock(jobMutex_);
    jobCv_.wait(lock, [this] { return openDone_; });
    if (!portOpen_.load()) {
        if (error) *error = openErrorCode_;
        if (errorDetail) *errorDetail = openErrorDetail_;
        lock.unlock();
        ioThread_.join();
        return false;
    }
    return true;
}

bool ModbusBusSession::openPortOnIoThread()
{
    serial_ = factory_ ? factory_() : makePlatformSerialPort();
    if (!serial_) {
        std::scoped_lock lock(jobMutex_);
        openErrorCode_ = BusError::PortUnavailable;
        openErrorDetail_ = "no serial port implementation";
        return false;
    }
    if (!serial_->openNamed(portName_, settings_)) {
        SPDLOG_ERROR("SerialBus: failed to open {}: {}", portName_, serial_->lastError());
        std::scoped_lock lock(jobMutex_);
        openErrorCode_ = classifyOpenError(serial_->lastSystemError());
        openErrorDetail_ = serial_->lastError();
        serial_.reset();
        return false;
    }
    SPDLOG_INFO("SerialBus: {} opened ({} {}{}{})", portName_, settings_.baudRate,
                settings_.dataBits, settings_.parity, settings_.stopBits);
    return true;
}

void ModbusBusSession::ioLoop()
{
    // The port is created, used, and destroyed only on this thread.
    const bool opened = openPortOnIoThread();
    portOpen_.store(opened);
    {
        std::scoped_lock lock(jobMutex_);
        openDone_ = true;
    }
    jobCv_.notify_all();
    if (!opened) {
        return;
    }

    lastBusActivity_ = std::chrono::steady_clock::now() - INTER_FRAME_DELAY;

    while (true) {
        std::unique_lock lock(jobMutex_);
        jobCv_.wait(lock, [this] { return jobPending_ || stopRequested_; });
        if (stopRequested_) {
            break;
        }
        const Bytes request = *jobRequest_;
        const int timeoutMs = jobTimeoutMs_;
        lock.unlock();

        Transaction result = runTransaction(request, timeoutMs);

        lock.lock();
        jobResult_ = std::move(result);
        jobPending_ = false;
        jobDone_ = true;
        lock.unlock();
        jobCv_.notify_all();
    }

    portOpen_.store(false);
    if (serial_) {
        if (serial_->isOpen()) {
            serial_->close();
        }
        serial_.reset();
    }
}

Transaction ModbusBusSession::transact(const Bytes& request, int timeoutMs)
{
    std::scoped_lock callLock(callMutex_);
    Transaction result;
    if (!portOpen_.load()) {
        result.error = BusError::NotOpen;
        return result;
    }
    std::unique_lock lock(jobMutex_);
    jobRequest_ = &request;
    jobTimeoutMs_ = timeoutMs;
    jobPending_ = true;
    jobDone_ = false;
    jobCv_.notify_all();
    jobCv_.wait(lock, [this] { return jobDone_; });
    jobRequest_ = nullptr;
    return jobResult_;
}

Transaction ModbusBusSession::runTransaction(const Bytes& request, int timeoutMs)
{
    Transaction result;

    if (!serial_ || !serial_->isOpen()) {
        result.error = BusError::NotOpen;
        return result;
    }
    if (request.size() < 2) {
        result.error = BusError::WriteFailed;
        return result;
    }

    // Drain stale bytes (e.g. a response that arrived after a previous
    // transaction's deadline) so they cannot be attributed to this request.
    const Bytes stale = serial_->readAll();
    if (!stale.empty()) {
        SPDLOG_DEBUG("SerialBus: {} discarded {} stale bytes: {}", portName_, stale.size(), hex(stale));
    }

    // RTU inter-frame silence, measured from the last bus activity so an
    // already-idle bus pays nothing.
    const auto sinceLast = std::chrono::steady_clock::now() - lastBusActivity_;
    if (sinceLast < INTER_FRAME_DELAY) {
        std::this_thread::sleep_for(INTER_FRAME_DELAY - sinceLast);
    }

    SPDLOG_DEBUG("SerialBus: {} TX [{}]: {}", portName_, request.size(), hex(request));

    if (serial_->write(request) != static_cast<int>(request.size())) {
        SPDLOG_ERROR("SerialBus: {} failed to write {} bytes", portName_, request.size());
        result.error = BusError::WriteFailed;
        return result;
    }
    if (!serial_->waitForBytesWritten(timeoutMs)) {
        SPDLOG_ERROR("SerialBus: {} write timeout", portName_);
        result.error = BusError::WriteFailed;
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    Bytes buffer;
    // When a complete frame from another slave address is discarded, remember
    // it so a final timeout is reported as WrongAddress, not a silent Timeout.
    bool sawWrongAddress = false;

    const auto finish = [&](BusError error) {
        lastBusActivity_ = std::chrono::steady_clock::now();
        result.error = error;
        return result;
    };
    const auto append = [](Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); };

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            SPDLOG_ERROR("SerialBus: {} timeout ({} bytes buffered): {}", portName_, buffer.size(), hex(buffer));
            return finish(sawWrongAddress ? BusError::WrongAddress : BusError::Timeout);
        }
        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int expected = modbus::expectedFrameLength(buffer);
        if (buffer.empty() || expected < 0 || static_cast<int>(buffer.size()) < expected) {
            if (!serial_->waitForReadyRead(std::max(1, remainingMs))) {
                continue; // deadline check at loop top decides
            }
            append(buffer, serial_->readAll());
        }

        const int frameLen = modbus::expectedFrameLength(buffer);
        if (frameLen == -1) {
            continue; // need more header bytes
        }
        if (frameLen == -2) {
            SPDLOG_ERROR("SerialBus: {} unframeable bytes: {}", portName_, hex(buffer));
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
                        portName_, frame[0], request[0]);
            buffer.erase(buffer.begin(), buffer.begin() + frameLen);
            sawWrongAddress = true;
            continue;
        }

        SPDLOG_DEBUG("SerialBus: {} RX [{}]: {}", portName_, frame.size(), hex(frame));

        switch (verdict) {
        case modbus::ResponseVerdict::Ok:
        case modbus::ResponseVerdict::Exception:
            break;
        case modbus::ResponseVerdict::CrcMismatch:
            SPDLOG_ERROR("SerialBus: {} CRC mismatch (possible duplicate-address collision): {}",
                         portName_, hex(frame));
            return finish(BusError::CrcError);
        case modbus::ResponseVerdict::WrongFunction:
            SPDLOG_ERROR("SerialBus: {} wrong function code in response: {}", portName_, hex(frame));
            return finish(BusError::WrongFunction);
        default:
            SPDLOG_ERROR("SerialBus: {} malformed response: {}", portName_, hex(frame));
            return finish(BusError::FrameError);
        }

        // Valid frame. Anything already buffered past it, or arriving in the
        // collision-listen window, means a second device answered too.
        buffer.erase(buffer.begin(), buffer.begin() + frameLen);
        if (buffer.empty() && serial_->waitForReadyRead(COLLISION_LISTEN_MS)) {
            append(buffer, serial_->readAll());
        }
        if (!buffer.empty()) {
            SPDLOG_ERROR("SerialBus: {} {} trailing bytes after a valid frame — "
                         "duplicate-address collision suspected: {}",
                         portName_, buffer.size(), hex(buffer));
            return finish(BusError::CollisionSuspected);
        }

        if (verdict == modbus::ResponseVerdict::Exception) {
            result.exceptionCode = frame[2];
            SPDLOG_ERROR("SerialBus: {} Modbus exception 0x{:02X} (func=0x{:02X}, addr={})",
                         portName_, result.exceptionCode, frame[1] & 0x7F, frame[0]);
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
