#pragma once

#include "backend/nanopositioner/oeabt/OeabtProtocol.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace backend::services {
class ISerialPort;
}

namespace backend::nanopositioner::oeabt {

struct SerialEndpoint {
    std::string persistentId;
    std::string systemPath;
    std::string displayName;
    std::string serialNumber;
    std::optional<std::uint16_t> vendorId;
    std::optional<std::uint16_t> productId;
    bool knownOeabtCandidate{false};
};

class SerialTransport final : public IByteTransport {
public:
    explicit SerialTransport(std::string systemPath);
    ~SerialTransport() override;

    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;

    Result<void> open();
    void close();
    bool isOpen() const;
    const std::string& systemPath() const { return systemPath_; }

    Result<void> clear() override;
    Result<void> write(std::string_view bytes) override;
    Result<std::string> readUntil(char delimiter, std::chrono::milliseconds timeout) override;

    static std::vector<SerialEndpoint> enumerateEndpoints();
    static std::optional<SerialEndpoint> resolveEndpoint(std::string_view persistentId);

private:
    static Error serialError(const backend::services::ISerialPort& port,
                             std::string_view operation);

    std::string systemPath_;
    std::unique_ptr<backend::services::ISerialPort> port_;
};

} // namespace backend::nanopositioner::oeabt
