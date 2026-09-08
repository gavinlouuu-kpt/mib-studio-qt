#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"

#include <QSerialPort>
#include <QSerialPortInfo>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace backend::services::serialbus {

namespace {
    // Modbus RTU inter-frame silence (3.5 char times: ~4 ms at 9600 baud),
    // measured from the last bus activity — an idle bus needs no extra wait.
    constexpr auto INTER_FRAME_DELAY = std::chrono::milliseconds(5);
    // After a complete valid frame, listen briefly for trailing bytes — a
    // second device answering the same address shows up here.
    constexpr int COLLISION_LISTEN_MS = 5;

    QSerialPort::Parity toQtParity(char parity)
    {
        switch (parity) {
        case 'E': return QSerialPort::EvenParity;
        case 'O': return QSerialPort::OddParity;
        default:  return QSerialPort::NoParity;
        }
    }

    QSerialPort::DataBits toQtDataBits(int bits)
    {
        switch (bits) {
        case 5: return QSerialPort::Data5;
        case 6: return QSerialPort::Data6;
        case 7: return QSerialPort::Data7;
        default: return QSerialPort::Data8;
        }
    }

    QSerialPort::StopBits toQtStopBits(int bits)
    {
        return bits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop;
    }

    // Classify an open() failure from the typed QSerialPort error, never from
    // the translated errorString text.
    BusError classifyOpenError(QSerialPort::SerialPortError error)
    {
        switch (error) {
        case QSerialPort::PermissionError:
            return BusError::PortBusy; // held by another process, or no rights
        case QSerialPort::DeviceNotFoundError:
        default:
            return BusError::PortUnavailable;
        }
    }
} // namespace

std::vector<PortInfo> availablePorts()
{
    std::vector<PortInfo> ports;
    const auto infos = QSerialPortInfo::availablePorts();
    ports.reserve(static_cast<size_t>(infos.size()));
    for (const QSerialPortInfo& info : infos) {
        PortInfo p;
        p.systemName = info.portName();
        p.systemLocation = info.systemLocation();
        p.description = info.description();
        p.manufacturer = info.manufacturer();
        p.serialNumber = info.serialNumber();
        p.vendorId = info.hasVendorIdentifier() ? info.vendorIdentifier() : 0;
        p.productId = info.hasProductIdentifier() ? info.productIdentifier() : 0;
        ports.push_back(std::move(p));
    }
    return ports;
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
ModbusBusSession::ModbusBusSession(const QString& portName, const SerialSettings& settings)
    : portName_(portName), settings_(settings)
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
    SPDLOG_INFO("SerialBus: session on {} closed", portName_.toStdString());
}

bool ModbusBusSession::start(BusError* error, QString* errorDetail)
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
    serial_ = new QSerialPort();
    serial_->setPortName(portName_);
    serial_->setBaudRate(settings_.baudRate);
    serial_->setDataBits(toQtDataBits(settings_.dataBits));
    serial_->setParity(toQtParity(settings_.parity));
    serial_->setStopBits(toQtStopBits(settings_.stopBits));
    serial_->setFlowControl(QSerialPort::NoFlowControl);

    if (!serial_->open(QIODevice::ReadWrite)) {
        SPDLOG_ERROR("SerialBus: failed to open {}: {}", portName_.toStdString(),
                     serial_->errorString().toStdString());
        std::scoped_lock lock(jobMutex_);
        openErrorCode_ = classifyOpenError(serial_->error());
        openErrorDetail_ = serial_->errorString();
        delete serial_;
        serial_ = nullptr;
        return false;
    }
    SPDLOG_INFO("SerialBus: {} opened ({} {}{}{})", portName_.toStdString(),
                settings_.baudRate, settings_.dataBits, settings_.parity, settings_.stopBits);
    return true;
}

void ModbusBusSession::ioLoop()
{
    // The QSerialPort is created, used, and destroyed only on this thread
    // (blocking waitFor* API, no event loop), so no other thread's Qt event
    // dispatch can ever touch its buffers.
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
        const QByteArray request = *jobRequest_;
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
        delete serial_;
        serial_ = nullptr;
    }
}

Transaction ModbusBusSession::transact(const QByteArray& request, int timeoutMs)
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

Transaction ModbusBusSession::runTransaction(const QByteArray& request, int timeoutMs)
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
    const QByteArray stale = serial_->readAll();
    if (!stale.isEmpty()) {
        SPDLOG_DEBUG("SerialBus: {} discarded {} stale bytes: {}", portName_.toStdString(),
                     stale.size(), stale.toHex(' ').constData());
    }

    // RTU inter-frame silence, measured from the last bus activity so an
    // already-idle bus pays nothing.
    const auto sinceLast = std::chrono::steady_clock::now() - lastBusActivity_;
    if (sinceLast < INTER_FRAME_DELAY) {
        std::this_thread::sleep_for(INTER_FRAME_DELAY - sinceLast);
    }

    SPDLOG_DEBUG("SerialBus: {} TX [{}]: {}", portName_.toStdString(),
                 request.size(), request.toHex(' ').constData());

    if (serial_->write(request) != request.size()) {
        SPDLOG_ERROR("SerialBus: {} failed to write {} bytes", portName_.toStdString(),
                     request.size());
        result.error = BusError::WriteFailed;
        return result;
    }
    if (!serial_->waitForBytesWritten(timeoutMs)) {
        SPDLOG_ERROR("SerialBus: {} write timeout", portName_.toStdString());
        result.error = BusError::WriteFailed;
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    QByteArray buffer;
    // When a complete frame from another slave address is discarded, remember
    // it so a final timeout is reported as WrongAddress, not a silent Timeout.
    bool sawWrongAddress = false;

    const auto finish = [&](BusError error) {
        lastBusActivity_ = std::chrono::steady_clock::now();
        result.error = error;
        return result;
    };

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            SPDLOG_ERROR("SerialBus: {} timeout ({} bytes buffered): {}", portName_.toStdString(),
                         buffer.size(), buffer.toHex(' ').constData());
            return finish(sawWrongAddress ? BusError::WrongAddress : BusError::Timeout);
        }
        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (buffer.isEmpty() || modbus::expectedFrameLength(buffer) < 0 ||
            buffer.size() < modbus::expectedFrameLength(buffer)) {
            if (!serial_->waitForReadyRead(std::max(1, remainingMs))) {
                continue; // deadline check at loop top decides
            }
            buffer.append(serial_->readAll());
        }

        const int frameLen = modbus::expectedFrameLength(buffer);
        if (frameLen == -1) {
            continue; // need more header bytes
        }
        if (frameLen == -2) {
            SPDLOG_ERROR("SerialBus: {} unframeable bytes: {}", portName_.toStdString(),
                         buffer.toHex(' ').constData());
            return finish(BusError::FrameError);
        }
        if (buffer.size() < frameLen) {
            continue; // frame incomplete
        }

        const QByteArray frame = buffer.left(frameLen);
        const auto verdict = modbus::classifyResponse(request, frame);
        if (verdict == modbus::ResponseVerdict::WrongAddress) {
            // Stale/delayed frame from another device: discard, keep reading.
            SPDLOG_WARN("SerialBus: {} discarding frame from addr {} while waiting on addr {}",
                        portName_.toStdString(), static_cast<uint8_t>(frame[0]),
                        static_cast<uint8_t>(request[0]));
            buffer.remove(0, frameLen);
            sawWrongAddress = true;
            continue;
        }

        SPDLOG_DEBUG("SerialBus: {} RX [{}]: {}", portName_.toStdString(),
                     frame.size(), frame.toHex(' ').constData());

        switch (verdict) {
        case modbus::ResponseVerdict::Ok:
        case modbus::ResponseVerdict::Exception:
            break;
        case modbus::ResponseVerdict::CrcMismatch:
            SPDLOG_ERROR("SerialBus: {} CRC mismatch (possible duplicate-address collision): {}",
                         portName_.toStdString(), frame.toHex(' ').constData());
            return finish(BusError::CrcError);
        case modbus::ResponseVerdict::WrongFunction:
            SPDLOG_ERROR("SerialBus: {} wrong function code in response: {}",
                         portName_.toStdString(), frame.toHex(' ').constData());
            return finish(BusError::WrongFunction);
        default:
            SPDLOG_ERROR("SerialBus: {} malformed response: {}", portName_.toStdString(),
                         frame.toHex(' ').constData());
            return finish(BusError::FrameError);
        }

        // Valid frame. Anything already buffered past it, or arriving in the
        // collision-listen window, means a second device answered too.
        buffer.remove(0, frameLen);
        if (buffer.isEmpty() && serial_->waitForReadyRead(COLLISION_LISTEN_MS)) {
            buffer.append(serial_->readAll());
        }
        if (!buffer.isEmpty()) {
            SPDLOG_ERROR("SerialBus: {} {} trailing bytes after a valid frame — "
                         "duplicate-address collision suspected: {}",
                         portName_.toStdString(), buffer.size(), buffer.toHex(' ').constData());
            return finish(BusError::CollisionSuspected);
        }

        if (verdict == modbus::ResponseVerdict::Exception) {
            result.exceptionCode = static_cast<uint8_t>(frame[2]);
            SPDLOG_ERROR("SerialBus: {} Modbus exception 0x{:02X} (func=0x{:02X}, addr={})",
                         portName_.toStdString(), result.exceptionCode,
                         static_cast<uint8_t>(frame[1]) & 0x7F,
                         static_cast<uint8_t>(frame[0]));
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
QString SerialBusManager::normalizeKey(const QString& portName)
{
    // "COM3", "\\.\COM3", "ttyUSB0" and "/dev/ttyUSB0" must map to one bus.
    QString key = portName.trimmed();
    key.remove(QStringLiteral("\\\\.\\"));
    const int slash = key.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        key = key.mid(slash + 1);
    }
    return key.toLower();
}

std::shared_ptr<ModbusBusSession> SerialBusManager::acquire(const QString& portName,
                                                            const SerialSettings& settings,
                                                            BusError* error,
                                                            QString* errorDetail)
{
    const QString key = normalizeKey(portName);
    std::scoped_lock lock(mutex_);

    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        if (auto existing = it->second.lock()) {
            if (existing->settings() != settings) {
                SPDLOG_ERROR("SerialBus: {} already open with different settings "
                             "({} vs requested {})", portName.toStdString(),
                             existing->settings().baudRate, settings.baudRate);
                if (error) *error = BusError::PortBusy;
                if (errorDetail) {
                    *errorDetail = QStringLiteral("port already in use with different serial settings");
                }
                return nullptr;
            }
            if (error) *error = BusError::None;
            return existing;
        }
        sessions_.erase(it);
    }

    auto* raw = new ModbusBusSession(portName, settings);
    BusError openError = BusError::PortUnavailable;
    QString detail;
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
