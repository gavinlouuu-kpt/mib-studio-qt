#include "backend/nanopositioner/oeabt/SerialTransport.h"

#include "backend/services/ISerialPort.h"
#include <chrono>

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

SerialTransport::SerialTransport(std::string systemPath)
    : systemPath_(std::move(systemPath)), port_(backend::services::makePlatformSerialPort()) {}
SerialTransport::~SerialTransport() {
    close();
}
Error SerialTransport::serialError(const backend::services::ISerialPort& port,
                                   std::string_view operation) {
    return Error{ErrorCode::IoError, std::string(operation) + ": " + port.lastError(), {}};
}
Result<void> SerialTransport::open() {
    if (isOpen()) return Result<void>::success();
    backend::services::SerialSettings settings;
    settings.baudRate = 115200;
    if (!port_->openNamed(systemPath_, settings))
        return Result<void>::failure(serialError(*port_, "Could not open OEABT endpoint"));
    return Result<void>::success();
}
void SerialTransport::close() {
    if (port_) port_->close();
}
bool SerialTransport::isOpen() const {
    return port_ && port_->isOpen();
}
Result<void> SerialTransport::clear() {
    if (!isOpen())
        return Result<void>::failure(Error{ErrorCode::IoError, "Serial port is closed", {}});
    (void)port_->readAll();
    return Result<void>::success();
}
Result<void> SerialTransport::write(std::string_view bytes) {
    const std::vector<uint8_t> data(bytes.begin(), bytes.end());
    if (port_->write(data) != static_cast<int>(data.size()) ||
        !port_->waitForBytesWritten(
            static_cast<int>(ControllerSession::kTransactionTimeout.count())))
        return Result<void>::failure(serialError(*port_, "Could not write OEABT command"));
    return Result<void>::success();
}
Result<std::string> SerialTransport::readUntil(char delimiter, std::chrono::milliseconds timeout) {
    if (!isOpen())
        return Result<std::string>::failure(Error{ErrorCode::IoError, "Serial port is closed", {}});
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string response;
    const std::string trailer = std::string("\x03\r", 2) + delimiter;
    do {
        const auto bytes = port_->readAll();
        response.append(bytes.begin(), bytes.end());
        // V0.5.4 emits diagnostic lines around ACKs. Extract only a full frame;
        // ControllerSession still strictly validates address/status/trailer.
        const auto start = response.find(static_cast<char>(0xff));
        const auto end =
            start == std::string::npos ? std::string::npos : response.find(trailer, start + 4);
        if (end != std::string::npos)
            return Result<std::string>::success(
                response.substr(start, end + trailer.size() - start));
        if (response.size() > 65536)
            return Result<std::string>::failure(
                Error{ErrorCode::ProtocolError, "OEABT response exceeds size limit", {}});
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) break;
        port_->waitForReadyRead(static_cast<int>(remaining));
    } while (std::chrono::steady_clock::now() < deadline);
    return Result<std::string>::failure(
        Error{ErrorCode::Timeout, "Timed out waiting for OEABT frame", response});
}

std::vector<SerialEndpoint> SerialTransport::enumerateEndpoints() {
    std::vector<SerialEndpoint> endpoints;
    const auto ports = backend::services::enumerateSerialPorts();
    endpoints.reserve(static_cast<std::size_t>(ports.size()));

    for (const auto& port : ports) {
        SerialEndpoint endpoint;
        endpoint.systemPath = port.systemLocation;
        endpoint.serialNumber = port.serialNumber;
        if (port.vendorId != 0) {
            endpoint.vendorId = port.vendorId;
        }
        if (port.productId != 0) {
            endpoint.productId = port.productId;
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

        endpoint.displayName = port.systemName;
        const auto description = port.description;
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

std::optional<SerialEndpoint> SerialTransport::resolveEndpoint(std::string_view persistentId) {
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
