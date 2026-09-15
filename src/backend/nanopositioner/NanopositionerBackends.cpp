#include "backend/nanopositioner/INanopositionerBackend.h"

#include "backend/nanopositioner/oeabt/OeabtProtocol.h"
#include "backend/nanopositioner/oeabt/SerialTransport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

#ifndef MIB_HAS_COREMOR
#define MIB_HAS_COREMOR 0
#endif

#if MIB_HAS_COREMOR
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Coremor/XMT_DLL_SER.h>
#endif

namespace backend::nanopositioner {
namespace {

constexpr double kCoremorMaximumVoltage = 250.0;

class OeabtBackend final : public INanopositionerBackend {
public:
    ~OeabtBackend() override { disconnect(); }

    BackendKind kind() const override { return BackendKind::Oeabt; }

    bool connect(const Endpoint& endpoint, std::string& error) override {
        disconnect();

        std::string path = endpoint.systemPath;
        if (!endpoint.persistentId.empty()) {
            if (const auto resolved =
                    oeabt::SerialTransport::resolveEndpoint(endpoint.persistentId)) {
                path = resolved->systemPath;
            }
        }
        if (path.empty()) {
            error = "OEABT endpoint has no resolvable serial path";
            return false;
        }

        transport_ = std::make_unique<oeabt::SerialTransport>(path);
        auto opened = transport_->open();
        if (!opened) {
            error = opened.error().message;
            disconnect();
            return false;
        }
        session_ = std::make_unique<oeabt::ControllerSession>(*transport_);

        auto identity = session_->identify();
        if (!identity) {
            error = identity.error().message;
            disconnect();
            return false;
        }
        auto capabilities = session_->readCapabilities();
        if (!capabilities) {
            error = capabilities.error().message;
            disconnect();
            return false;
        }
        auto limits =
            session_->setSafetyLimits(oeabt::VoltageMv{0}, capabilities.value().maxVoltage);
        if (!limits) {
            error = limits.error().message;
            disconnect();
            return false;
        }
        auto voltage = session_->readVoltage();
        if (!voltage) {
            error = voltage.error().message;
            disconnect();
            return false;
        }

        maximumVoltage_ = static_cast<double>(capabilities.value().maxVoltage.value) / 1000.0;
        connectedPath_ = path;
        connected_ = true;
        return true;
    }

    void disconnect() override {
        connected_ = false;
        session_.reset();
        if (transport_) {
            transport_->close();
            transport_.reset();
        }
        connectedPath_.clear();
        maximumVoltage_.reset();
    }

    bool isConnected() const override { return connected_; }

    bool readVoltage(double& volts, std::string& error) override {
        if (!connected_ || !session_) {
            error = "OEABT controller is not connected";
            return false;
        }
        auto voltage = session_->readVoltage();
        if (!voltage) {
            error = voltage.error().message;
            return false;
        }
        volts = static_cast<double>(voltage.value().value) / 1000.0;
        return true;
    }

    bool setVoltage(double volts, std::string& error) override {
        if (!connected_ || !session_) {
            error = "OEABT controller is not connected";
            return false;
        }
        const double millivolts = std::trunc(volts * 1000.0);
        if (!std::isfinite(volts) || millivolts < 0.0 ||
            millivolts > std::numeric_limits<std::int32_t>::max()) {
            error = "OEABT voltage cannot be represented in millivolts";
            return false;
        }
        auto result = session_->setVoltage(oeabt::VoltageMv{static_cast<std::int32_t>(millivolts)});
        if (!result) {
            error = result.error().message;
            return false;
        }
        return true;
    }

    std::optional<double> maximumVoltage() const override { return maximumVoltage_; }
    std::string connectedEndpoint() const override { return connectedPath_; }

private:
    std::unique_ptr<oeabt::SerialTransport> transport_;
    std::unique_ptr<oeabt::ControllerSession> session_;
    std::optional<double> maximumVoltage_;
    std::string connectedPath_;
    bool connected_{false};
};

#if MIB_HAS_COREMOR
std::mutex& coremorMutex() {
    static std::mutex mutex;
    return mutex;
}

bool plausibleCoremorVoltage(double value) {
    return std::isfinite(value) && value >= -0.05 && value <= kCoremorMaximumVoltage;
}

int openCoremorWithRetries(int port, int baudRate) {
    constexpr int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        CloseSer();
        if (const int result = OpenComConnectRS232(port, baudRate); result != 0) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return 0;
}
#endif

class CoremorBackend final : public INanopositionerBackend {
public:
    ~CoremorBackend() override { disconnect(); }

    BackendKind kind() const override { return BackendKind::Coremor; }

    bool connect(const Endpoint& endpoint, std::string& error) override {
#if MIB_HAS_COREMOR
        disconnect();
        if (endpoint.coremorPort <= 0) {
            error = "CoreMOR endpoint has no COM port number";
            return false;
        }
        std::scoped_lock lock(coremorMutex());
        if (openCoremorWithRetries(endpoint.coremorPort, endpoint.coremorBaudRate) == 0) {
            error = "Could not open CoreMOR COM port";
            CloseSer();
            return false;
        }
        port_ = endpoint.coremorPort;
        address_ = endpoint.coremorAddress;
        const double voltage = XMT_COMMAND_ReadData(address_, 5, 0, 0);
        if (!plausibleCoremorVoltage(voltage)) {
            error = "CoreMOR returned an implausible voltage";
            CloseSer();
            port_ = -1;
            return false;
        }
        connected_ = true;
        return true;
#else
        (void)endpoint;
        error = "CoreMOR SDK support is not available in this build";
        return false;
#endif
    }

    void disconnect() override {
#if MIB_HAS_COREMOR
        if (connected_) {
            std::scoped_lock lock(coremorMutex());
            CloseSer();
        }
#endif
        connected_ = false;
        port_ = -1;
    }

    bool isConnected() const override { return connected_; }

    bool readVoltage(double& volts, std::string& error) override {
#if MIB_HAS_COREMOR
        if (!connected_) {
            error = "CoreMOR controller is not connected";
            return false;
        }
        std::scoped_lock lock(coremorMutex());
        const double value = XMT_COMMAND_ReadData(address_, 5, 0, 0);
        if (!plausibleCoremorVoltage(value)) {
            error = "CoreMOR returned an implausible voltage";
            return false;
        }
        volts = value;
        return true;
#else
        (void)volts;
        error = "CoreMOR SDK support is not available in this build";
        return false;
#endif
    }

    bool setVoltage(double volts, std::string& error) override {
#if MIB_HAS_COREMOR
        if (!connected_) {
            error = "CoreMOR controller is not connected";
            return false;
        }
        if (!plausibleCoremorVoltage(volts) || volts < 0.0) {
            error = "CoreMOR voltage is outside the supported range";
            return false;
        }
        std::scoped_lock lock(coremorMutex());
        XMT_COMMAND_SinglePoint(address_, 0, 0, 0, volts);
        return true;
#else
        (void)volts;
        error = "CoreMOR SDK support is not available in this build";
        return false;
#endif
    }

    std::optional<double> maximumVoltage() const override {
        return MIB_HAS_COREMOR ? std::optional<double>(kCoremorMaximumVoltage) : std::nullopt;
    }

    std::string connectedEndpoint() const override {
        return port_ > 0 ? "COM" + std::to_string(port_) : std::string{};
    }

private:
    int port_{-1};
    std::uint8_t address_{1};
    bool connected_{false};
};

// Native serial handles have no Qt thread affinity. Serialize whole backend
// operations, not individual reads/writes, so concurrent callers cannot mix frames.
class SerializedBackendProxy final : public INanopositionerBackend {
public:
    explicit SerializedBackendProxy(std::unique_ptr<INanopositionerBackend> backend)
        : backend_(std::move(backend)), kind_(backend_->kind()) {}
    BackendKind kind() const override { return kind_; }
    bool connect(const Endpoint& endpoint, std::string& error) override {
        std::scoped_lock lock(mutex_);
        return backend_->connect(endpoint, error);
    }
    void disconnect() override {
        std::scoped_lock lock(mutex_);
        backend_->disconnect();
    }
    bool isConnected() const override {
        std::scoped_lock lock(mutex_);
        return backend_->isConnected();
    }
    bool readVoltage(double& volts, std::string& error) override {
        std::scoped_lock lock(mutex_);
        return backend_->readVoltage(volts, error);
    }
    bool setVoltage(double volts, std::string& error) override {
        std::scoped_lock lock(mutex_);
        return backend_->setVoltage(volts, error);
    }
    std::optional<double> maximumVoltage() const override {
        std::scoped_lock lock(mutex_);
        return backend_->maximumVoltage();
    }
    std::string connectedEndpoint() const override {
        std::scoped_lock lock(mutex_);
        return backend_->connectedEndpoint();
    }

private:
    std::unique_ptr<INanopositionerBackend> backend_;
    BackendKind kind_;
    mutable std::mutex mutex_;
};

std::unique_ptr<INanopositionerBackend> createUnthreadedBackend(BackendKind kind) {
    switch (kind) {
    case BackendKind::Oeabt:
        return std::make_unique<OeabtBackend>();
    case BackendKind::Coremor:
        return std::make_unique<CoremorBackend>();
    case BackendKind::Auto:
        break;
    }
    return nullptr;
}

} // namespace

const char* backendKindName(BackendKind kind) {
    switch (kind) {
    case BackendKind::Auto:
        return "auto";
    case BackendKind::Oeabt:
        return "oeabt";
    case BackendKind::Coremor:
        return "coremor";
    }
    return "auto";
}

std::optional<BackendKind> parseBackendKind(std::string_view value) {
    if (value == "auto") return BackendKind::Auto;
    if (value == "oeabt") return BackendKind::Oeabt;
    if (value == "coremor") return BackendKind::Coremor;
    return std::nullopt;
}

PersistedSelection resolvePersistedSelection(const std::optional<std::string>& backendName,
                                             const std::optional<std::string>& endpointId,
                                             const std::optional<int>& legacyComPort) {
    PersistedSelection selection;
    if (backendName) {
        if (const auto parsed = parseBackendKind(*backendName)) {
            selection.backend = *parsed;
        }
    }
    if (endpointId) {
        selection.endpointId = *endpointId;
    }
    if (legacyComPort && *legacyComPort > 0) {
        selection.legacyComPort = *legacyComPort;
        if (!backendName) {
            selection.backend = BackendKind::Coremor;
            selection.endpointId = "COM" + std::to_string(*legacyComPort);
            selection.migratedLegacyComPort = true;
        }
    }
    return selection;
}

std::vector<Endpoint> availableOeabtEndpoints() {
    std::vector<Endpoint> endpoints;
    for (const auto& serial : oeabt::SerialTransport::enumerateEndpoints()) {
        Endpoint endpoint;
        endpoint.backend = BackendKind::Oeabt;
        endpoint.persistentId = serial.persistentId;
        endpoint.systemPath = serial.systemPath;
        endpoint.displayName = serial.displayName;
        endpoint.vendorId = serial.vendorId;
        endpoint.productId = serial.productId;
        endpoint.knownOeabtCandidate = serial.knownOeabtCandidate;
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

std::unique_ptr<INanopositionerBackend> createNanopositionerBackend(BackendKind kind) {
    auto backend = createUnthreadedBackend(kind);
    return backend ? std::make_unique<SerializedBackendProxy>(std::move(backend)) : nullptr;
}

bool probeNanopositionerEndpoint(const Endpoint& endpoint, std::string& error) {
    BackendKind kind = endpoint.backend;
    if (kind == BackendKind::Auto) {
        kind = endpoint.coremorPort > 0 && !endpoint.knownOeabtCandidate ? BackendKind::Coremor
                                                                         : BackendKind::Oeabt;
    }
    auto backend = createNanopositionerBackend(kind);
    if (!backend) {
        error = "Could not select nanopositioner backend";
        return false;
    }
    if (!backend->connect(endpoint, error)) {
        return false;
    }
    backend->disconnect();
    return true;
}

} // namespace backend::nanopositioner
