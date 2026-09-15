#include "backend/nanopositioner/oeabt/QtSerialTransport.h"

#include <QElapsedTimer>
#include <QSerialPort>
#include <QSerialPortInfo>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace backend::nanopositioner::oeabt {
namespace {

bool isKnownCandidate(std::uint16_t vendor, std::uint16_t product) {
    return (vendor == 0x1a86 &&
            (product == 0x7523 || product == 0x5523 || product == 0x7522 || product == 0xe523)) ||
           (vendor == 0x4348 && product == 0x5523);
}

std::string usbIdentity(std::uint16_t vendor, std::uint16_t product, const std::string& serial) {
    std::ostringstream out;
    out << "usb:" << std::hex << std::setfill('0') << std::setw(4) << vendor << ':' << std::setw(4)
        << product << ':' << serial;
    return out.str();
}

#ifdef __linux__
std::string persistentLinuxPath(const std::string& systemPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path canonicalDevice = fs::canonical(systemPath, ec);
    if (ec) {
        return {};
    }

    // by-path is preferred because generic CH341 devices frequently report
    // the same serial string and collide in /dev/serial/by-id.
    for (const char* directory : {"/dev/serial/by-path", "/dev/serial/by-id"}) {
        ec.clear();
        if (!fs::is_directory(directory, ec)) {
            continue;
        }
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code itemError;
            const auto target = fs::canonical(it->path(), itemError);
            if (!itemError && target == canonicalDevice) {
                return it->path().string();
            }
        }
    }
    return {};
}
#endif

} // namespace

QtSerialTransport::QtSerialTransport(std::string systemPath)
    : systemPath_(std::move(systemPath)), port_(std::make_unique<QSerialPort>()) {}

QtSerialTransport::~QtSerialTransport() {
    close();
}

Error QtSerialTransport::serialError(const QSerialPort& port, std::string_view operation) {
    ErrorCode code = ErrorCode::IoError;
    switch (port.error()) {
    case QSerialPort::PermissionError:
    case QSerialPort::OpenError:
        code = ErrorCode::Busy;
        break;
    case QSerialPort::TimeoutError:
        code = ErrorCode::Timeout;
        break;
    default:
        break;
    }
    return Error{code, std::string(operation) + ": " + port.errorString().toStdString(), {}};
}

Result<void> QtSerialTransport::open() {
    if (port_->isOpen()) {
        return Result<void>::success();
    }

    port_->setPortName(QString::fromStdString(systemPath_));
    if (!port_->open(QIODevice::ReadWrite)) {
        return Result<void>::failure(serialError(*port_, "Could not open serial endpoint"));
    }
    if (!port_->setBaudRate(QSerialPort::Baud115200) || !port_->setDataBits(QSerialPort::Data8) ||
        !port_->setParity(QSerialPort::NoParity) || !port_->setStopBits(QSerialPort::OneStop) ||
        !port_->setFlowControl(QSerialPort::NoFlowControl)) {
        const auto error = serialError(*port_, "Could not configure OEABT serial endpoint");
        port_->close();
        return Result<void>::failure(error);
    }
    return Result<void>::success();
}

void QtSerialTransport::close() {
    if (port_ && port_->isOpen()) {
        port_->close();
    }
}

bool QtSerialTransport::isOpen() const {
    return port_ && port_->isOpen();
}

Result<void> QtSerialTransport::clear() {
    if (!isOpen()) {
        return Result<void>::failure(
            Error{ErrorCode::IoError, "OEABT serial endpoint is not open", {}});
    }
    if (!port_->clear(QSerialPort::AllDirections)) {
        return Result<void>::failure(serialError(*port_, "Could not clear serial endpoint"));
    }
    return Result<void>::success();
}

Result<void> QtSerialTransport::write(std::string_view bytes) {
    if (!isOpen()) {
        return Result<void>::failure(
            Error{ErrorCode::IoError, "OEABT serial endpoint is not open", {}});
    }

    const QByteArray data(bytes.data(), static_cast<qsizetype>(bytes.size()));
    if (port_->write(data) != data.size()) {
        return Result<void>::failure(serialError(*port_, "Could not queue OEABT command"));
    }
    if (!port_->waitForBytesWritten(
            static_cast<int>(ControllerSession::kTransactionTimeout.count()))) {
        return Result<void>::failure(serialError(*port_, "Timed out writing OEABT command"));
    }
    return Result<void>::success();
}

Result<std::string> QtSerialTransport::readUntil(char delimiter,
                                                 std::chrono::milliseconds timeout) {
    if (!isOpen()) {
        return Result<std::string>::failure(
            Error{ErrorCode::IoError, "OEABT serial endpoint is not open", {}});
    }

    QElapsedTimer timer;
    timer.start();
    QByteArray response;
    while (timer.elapsed() < timeout.count()) {
        response.append(port_->readAll());
        // V0.5.4 emits unframed diagnostic lines around mode acknowledgements.
        // Wait for a complete frame, not merely the first debug newline. Core
        // parsing still validates the address, status and framing strictly.
        const auto start = response.indexOf(static_cast<char>(0xff));
        const QByteArray trailer = QByteArray("\x03\r", 2) + delimiter;
        const auto end = start < 0 ? -1 : response.indexOf(trailer, start + 4);
        if (start >= 0 && end >= 0) {
            return Result<std::string>::success(
                response.mid(start, end + trailer.size() - start).toStdString());
        }
        if (response.size() > 65536) {
            return Result<std::string>::failure(
                Error{ErrorCode::ProtocolError, "OEABT response exceeds size limit", {}});
        }

        const auto remaining = timeout.count() - timer.elapsed();
        const int waitMs = static_cast<int>(std::min<qint64>(100, remaining));
        if (waitMs <= 0) {
            break;
        }
        if (!port_->waitForReadyRead(waitMs) && port_->error() != QSerialPort::TimeoutError &&
            port_->error() != QSerialPort::NoError) {
            return Result<std::string>::failure(
                serialError(*port_, "Could not read OEABT response"));
        }
    }

    response.append(port_->readAll());
    if (!response.isEmpty()) {
        return Result<std::string>::failure(
            Error{ErrorCode::Timeout, "Timed out before OEABT response terminator",
                  std::string(response.constData(), static_cast<std::size_t>(response.size()))});
    }
    return Result<std::string>::failure(
        Error{ErrorCode::Timeout, "OEABT controller returned no bytes", {}});
}

std::vector<SerialEndpoint> QtSerialTransport::enumerateEndpoints() {
    std::vector<SerialEndpoint> endpoints;
    const auto ports = QSerialPortInfo::availablePorts();
    endpoints.reserve(static_cast<std::size_t>(ports.size()));

    for (const auto& port : ports) {
        SerialEndpoint endpoint;
        endpoint.systemPath = port.systemLocation().toStdString();
        endpoint.serialNumber = port.serialNumber().toStdString();
        if (port.hasVendorIdentifier()) {
            endpoint.vendorId = port.vendorIdentifier();
        }
        if (port.hasProductIdentifier()) {
            endpoint.productId = port.productIdentifier();
        }
        if (endpoint.vendorId && endpoint.productId) {
            endpoint.knownOeabtCandidate =
                isKnownCandidate(*endpoint.vendorId, *endpoint.productId);
        }

#ifdef __linux__
        endpoint.persistentId = persistentLinuxPath(endpoint.systemPath);
#endif
        if (endpoint.persistentId.empty() && endpoint.vendorId && endpoint.productId &&
            !endpoint.serialNumber.empty()) {
            endpoint.persistentId =
                usbIdentity(*endpoint.vendorId, *endpoint.productId, endpoint.serialNumber);
        }
        if (endpoint.persistentId.empty()) {
            endpoint.persistentId = endpoint.systemPath;
        }

        endpoint.displayName = port.portName().toStdString();
        const auto description = port.description().toStdString();
        if (!description.empty()) {
            endpoint.displayName += " — " + description;
        }
        endpoint.displayName += " (" + endpoint.systemPath + ')';
        endpoints.push_back(std::move(endpoint));
    }

    std::sort(endpoints.begin(), endpoints.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.systemPath < rhs.systemPath; });
    return endpoints;
}

std::optional<SerialEndpoint> QtSerialTransport::resolveEndpoint(std::string_view persistentId) {
    const auto endpoints = enumerateEndpoints();
    const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
        return endpoint.persistentId == persistentId || endpoint.systemPath == persistentId;
    });
    if (found == endpoints.end()) {
        return std::nullopt;
    }
    return *found;
}

} // namespace backend::nanopositioner::oeabt
