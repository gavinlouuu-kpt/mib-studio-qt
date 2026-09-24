#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace backend::nanopositioner {

enum class BackendKind {
    Auto,
    Oeabt,
    Coremor,
};

const char* backendKindName(BackendKind kind);
std::optional<BackendKind> parseBackendKind(std::string_view value);

struct PersistedSelection {
    BackendKind backend{BackendKind::Auto};
    std::string endpointId;
    int legacyComPort{-1};
    bool migratedLegacyComPort{false};
};

PersistedSelection resolvePersistedSelection(const std::optional<std::string>& backendName,
                                             const std::optional<std::string>& endpointId,
                                             const std::optional<int>& legacyComPort);

struct Endpoint {
    BackendKind backend{BackendKind::Auto};
    std::string persistentId;
    std::string systemPath;
    std::string displayName;
    std::optional<std::uint16_t> vendorId;
    std::optional<std::uint16_t> productId;
    bool knownOeabtCandidate{false};
    int coremorPort{-1};
    int coremorBaudRate{115200};
    std::uint8_t coremorAddress{1};
};

class INanopositionerBackend {
public:
    virtual ~INanopositionerBackend() = default;

    virtual BackendKind kind() const = 0;
    virtual bool connect(const Endpoint& endpoint, std::string& error) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool readVoltage(double& volts, std::string& error) = 0;
    virtual bool setVoltage(double volts, std::string& error) = 0;
    virtual std::optional<double> maximumVoltage() const = 0;
    virtual std::string connectedEndpoint() const = 0;
};

std::vector<Endpoint> availableOeabtEndpoints();
std::unique_ptr<INanopositionerBackend> createNanopositionerBackend(BackendKind kind);
bool probeNanopositionerEndpoint(const Endpoint& endpoint, std::string& error);

} // namespace backend::nanopositioner
