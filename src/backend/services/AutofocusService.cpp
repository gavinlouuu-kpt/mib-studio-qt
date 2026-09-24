#include "backend/services/AutofocusService.h"
#include "backend/services/AutofocusMath.h"
#include "backend/app/Tools.h"
#include "backend/diagnostics/CrashStateMirror.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <thread>

namespace backend::services {

namespace {
backend::nanopositioner::Endpoint legacyCoremorEndpoint(int comPort, int baudRate,
                                                        unsigned char deviceAddress) {
    backend::nanopositioner::Endpoint endpoint;
    endpoint.backend = backend::nanopositioner::BackendKind::Coremor;
    endpoint.persistentId = "COM" + std::to_string(comPort);
    endpoint.systemPath = endpoint.persistentId;
    endpoint.displayName = "CoreMOR on " + endpoint.persistentId;
    endpoint.coremorPort = comPort;
    endpoint.coremorBaudRate = baudRate;
    endpoint.coremorAddress = deviceAddress;
    return endpoint;
}
} // namespace

AutofocusService::AutofocusService()
    : AutofocusService([](backend::nanopositioner::BackendKind kind) {
          return backend::nanopositioner::createNanopositionerBackend(kind);
      }) {}

AutofocusService::AutofocusService(BackendFactory backendFactory)
    : backendFactory_(std::move(backendFactory)) {
    // statsThread_ runs for the lifetime of the service so that ring-ratio
    // samples accepted via onRingRatio (called from the ProcessingService
    // realtime thread on every valid frame) can be drained off the realtime
    // thread. It's independent of connect() / disconnect() because the UI
    // continues to read statistics even when the nanopositioner is not
    // connected.
    statsRunning_.store(true);
    statsThread_ = std::thread(&AutofocusService::statsLoop, this);
}

AutofocusService::~AutofocusService() {
    disconnect();
    // Stop stats thread
    if (statsRunning_.exchange(false)) {
        pendingSamplesCV_.notify_all();
        if (statsThread_.joinable()) statsThread_.join();
    }
}

bool AutofocusService::setBackendFactory(BackendFactory backendFactory) {
    if (connected_.load()) return false;
    backendFactory_ = std::move(backendFactory);
    return true;
}

bool AutofocusService::connect(const backend::nanopositioner::Endpoint& requestedEndpoint) {
    if (connected_.load()) {
        disconnect();
    }

    auto endpoint = requestedEndpoint;
    auto selectedKind = endpoint.backend;
    if (selectedKind == backend::nanopositioner::BackendKind::Auto) {
        selectedKind = endpoint.coremorPort > 0 && !endpoint.knownOeabtCandidate
                           ? backend::nanopositioner::BackendKind::Coremor
                           : backend::nanopositioner::BackendKind::Oeabt;
    }
    endpoint.backend = selectedKind;

    auto device = backendFactory_ ? backendFactory_(selectedKind) : nullptr;
    if (!device) {
        notifyStatus("No nanopositioner backend is available for the selected endpoint");
        return false;
    }

    std::string error;
    if (!device->connect(endpoint, error)) {
        SPDLOG_ERROR("AutofocusService: {} connection failed for {}: {}",
                     backend::nanopositioner::backendKindName(selectedKind),
                     endpoint.displayName.empty() ? endpoint.systemPath : endpoint.displayName,
                     error);
        notifyStatus(error);
        return false;
    }

    double voltage = 0.0;
    if (!device->readVoltage(voltage, error)) {
        device->disconnect();
        notifyStatus(error);
        return false;
    }
    currentVoltage_.store(voltage);
    {
        std::scoped_lock deviceLock(deviceMutex_);
        device_ = std::move(device);
        endpoint_ = endpoint;
        endpoint_.systemPath = device_->connectedEndpoint();
    }

    comPort_ = endpoint.coremorPort;
    baudRate_ = endpoint.coremorBaudRate;
    deviceAddress_ = endpoint.coremorAddress;
    activeControlSession_.store(false);
    connected_.store(true);

    {
        auto& mirror = backend::diagnostics::CrashStateMirror::instance().autofocus;
        mirror.connected.store(true);
        mirror.voltage.store(currentVoltage_.load());
        backend::diagnostics::CrashStateMirror::instance().setAutofocusPort(endpoint_.systemPath);
    }

    if (!running_.load()) {
        running_.store(true);
        controlThread_ = std::thread(&AutofocusService::controlLoop, this);
    }

    SPDLOG_INFO("AutofocusService: Connected to {} nanopositioner on {} at {:.3f} V (observe-only)",
                backend::nanopositioner::backendKindName(selectedKind), endpoint_.systemPath,
                currentVoltage_.load());
    notifyStatus("Connected to " +
                 std::string(backend::nanopositioner::backendKindName(selectedKind)) +
                 " nanopositioner on " + endpoint_.systemPath);
    return true;
}

bool AutofocusService::connect(int comPort, int baudRate, unsigned char deviceAddress) {
    return connect(legacyCoremorEndpoint(comPort, baudRate, deviceAddress));
}

void AutofocusService::disconnect() {
    if (!connected_.load()) {
        return;
    }

    // Stop control thread
    if (running_.load()) {
        running_.store(false);
        if (controlThread_.joinable()) {
            controlThread_.join();
        }
    }

    if (activeControlSession_.load()) {
        double safeVoltage = 0.0;
        {
            std::scoped_lock cfgLock(configMutex_);
            safeVoltage = config_.safeShutdownVoltage;
        }
        std::string error;
        if (!writeDeviceVoltage(safeVoltage, error)) {
            SPDLOG_ERROR("AutofocusService: Failed to apply safe shutdown voltage: {}", error);
            notifyStatus("Failed to apply safe shutdown voltage: " + error);
        }
    }

    {
        std::scoped_lock deviceLock(deviceMutex_);
        if (device_) {
            device_->disconnect();
            device_.reset();
        }
    }
    connected_.store(false);
    activeControlSession_.store(false);
    backend::diagnostics::CrashStateMirror::instance().autofocus.connected.store(false);

    // Clear buffers (both pending inbox and ring-ratio buffer) so a later
    // reconnect starts from a clean slate.
    {
        std::scoped_lock lock(pendingSamplesMutex_, ringRatioMutex_);
        pendingSamples_.clear();
        ringRatioBuffer_.clear();
        ringRatioTimestamps_.clear();
        ringRatioSequence_.store(0);
        lastRingRatioTimestampNs_.store(0);
        lastRingRatioUpdateUs_.store(0, std::memory_order_relaxed);
    }

    SPDLOG_INFO("AutofocusService: Disconnected from nanopositioner");
    notifyStatus("Disconnected from nanopositioner");
}

bool AutofocusService::probeComPort(int comPort, int baudRate, unsigned char deviceAddress) {
    return probeEndpoint(legacyCoremorEndpoint(comPort, baudRate, deviceAddress));
}

std::vector<backend::nanopositioner::Endpoint> AutofocusService::availableEndpoints() {
    auto endpoints = backend::nanopositioner::availableOeabtEndpoints();
    endpoints.erase(
        std::remove_if(endpoints.begin(), endpoints.end(),
                       [](const auto& endpoint) { return !endpoint.knownOeabtCandidate; }),
        endpoints.end());

    for (int port : backend::Tools::availableComPortNumbers()) {
        endpoints.push_back(legacyCoremorEndpoint(port, 115200, 1));
    }
    return endpoints;
}

bool AutofocusService::probeEndpoint(const backend::nanopositioner::Endpoint& endpoint) {
    std::string error;
    const bool ok = backend::nanopositioner::probeNanopositionerEndpoint(endpoint, error);
    if (!ok) {
        SPDLOG_DEBUG("AutofocusService: probe failed for {}: {}",
                     endpoint.displayName.empty() ? endpoint.systemPath : endpoint.displayName,
                     error);
    }
    return ok;
}

backend::nanopositioner::BackendKind AutofocusService::getBackendKind() const {
    std::scoped_lock lock(deviceMutex_);
    return device_ ? device_->kind() : endpoint_.backend;
}

std::string AutofocusService::getEndpointId() const {
    std::scoped_lock lock(deviceMutex_);
    return endpoint_.persistentId.empty() ? endpoint_.systemPath : endpoint_.persistentId;
}

bool AutofocusService::readDeviceVoltage(double& voltage, std::string& error) {
    std::scoped_lock lock(deviceMutex_);
    return device_ && device_->readVoltage(voltage, error);
}

bool AutofocusService::writeDeviceVoltage(double voltage, std::string& error) {
    Config cfg;
    {
        std::scoped_lock cfgLock(configMutex_);
        cfg = config_;
    }
    if (!std::isfinite(voltage) || voltage < cfg.minVoltage || voltage > cfg.maxVoltage) {
        error = "Requested voltage is outside configured safety limits";
        return false;
    }

    std::scoped_lock lock(deviceMutex_);
    if (!device_) {
        error = "Nanopositioner is not connected";
        return false;
    }
    if (const auto maximum = device_->maximumVoltage(); maximum && voltage > *maximum) {
        error = "Requested voltage exceeds the controller-reported maximum";
        return false;
    }
    if (!device_->setVoltage(voltage, error)) {
        return false;
    }
    activeControlSession_.store(true);
    currentVoltage_.store(voltage);
    backend::diagnostics::CrashStateMirror::instance().autofocus.voltage.store(voltage);
    return true;
}

void AutofocusService::setEnabled(bool enabled) {
    enabled_.store(enabled);
    backend::diagnostics::CrashStateMirror::instance().autofocus.enabled.store(enabled);
    SPDLOG_INFO("AutofocusService: Autofocus {}", enabled ? "enabled" : "disabled");
    notifyStatus(enabled ? "Autofocus enabled" : "Autofocus disabled");
}

void AutofocusService::increaseVoltage() {
    if (!connected_.load()) {
        return;
    }
    increaseVoltageRequest_.store(true);
}

void AutofocusService::decreaseVoltage() {
    if (!connected_.load()) {
        return;
    }
    decreaseVoltageRequest_.store(true);
}

void AutofocusService::setConfig(const Config& config) {
    std::scoped_lock lock(configMutex_);
    config_ = config;
}

AutofocusService::Config AutofocusService::getConfig() const {
    std::scoped_lock lock(configMutex_);
    return config_;
}

void AutofocusService::onRingRatio(double ringRatio, int64_t timestampNs) {
    // Called on the ProcessingService realtime thread on every valid frame
    // — keep this function O(1) and allocation-light. Heavy work (deque
    // trim, sort, stats refresh) happens on statsThread_.
    if (ringRatio <= 0.0) {
        return;
    }

    {
        std::scoped_lock lk(pendingSamplesMutex_);
        pendingSamples_.push_back({ringRatio, timestampNs});
    }

    // Freshness + sequence markers are updated here (not in statsLoop) so
    // the control loop and UI see data arrival immediately, even if the
    // stats thread is briefly behind.
    ringRatioSequence_.fetch_add(1, std::memory_order_relaxed);
    lastRingRatioTimestampNs_.store(timestampNs, std::memory_order_relaxed);
    lastRingRatioUpdateUs_.store(backend::Tools::getTimestamp(), std::memory_order_relaxed);

    pendingSamplesCV_.notify_one();
}

void AutofocusService::updateStatistics() {
    if (ringRatioBuffer_.empty()) {
        medianRingRatio_.store(0.0);
        averageRingRatio_.store(0.0);
        minRingRatio_.store(0.0);
        maxRingRatio_.store(0.0);
        return;
    }

    std::vector<double> sorted =
        std::vector<double>(ringRatioBuffer_.begin(), ringRatioBuffer_.end());
    std::sort(sorted.begin(), sorted.end());

    double median = calculateMedian(sorted);
    double sum = 0.0;
    double minVal = sorted.front();
    double maxVal = sorted.back();

    for (double val : sorted) {
        sum += val;
    }
    double avg = sum / sorted.size();

    medianRingRatio_.store(median);
    averageRingRatio_.store(avg);
    minRingRatio_.store(minVal);
    maxRingRatio_.store(maxVal);
}

double AutofocusService::calculateMedian(const std::vector<double>& sorted) const {
    if (sorted.empty()) return 0.0;
    size_t n = sorted.size();
    if (n % 2 == 0) {
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    } else {
        return sorted[n / 2];
    }
}

void AutofocusService::setStatusCallback(StatusCallback callback) {
    std::scoped_lock lock(callbackMutex_);
    statusCallback_ = std::move(callback);
}

void AutofocusService::notifyStatus(const std::string& message) const {
    StatusCallback callback;
    {
        std::scoped_lock lock(callbackMutex_);
        callback = statusCallback_;
    }
    if (callback) {
        callback(message);
    }
}

void AutofocusService::statsLoop() {
    SPDLOG_INFO("AutofocusService: Stats loop started");

    // Local drain buffer — swapped with pendingSamples_ under the pending
    // mutex so the realtime-thread producer is blocked only for the swap
    // itself (O(1) pointer swap).
    std::vector<PendingSample> drained;
    drained.reserve(1024);

    // Bound the wake rate so the sort cost amortises across samples. At
    // 5 kfps this batches ~50 samples per drain; at UI rates it drains
    // single samples. Autofocus control runs at ~20 Hz so stats freshness
    // of 10 ms is well under what the control loop can act on.
    constexpr auto kMinDrainInterval = std::chrono::milliseconds(10);

    while (statsRunning_.load()) {
        {
            std::unique_lock<std::mutex> lk(pendingSamplesMutex_);
            pendingSamplesCV_.wait(
                lk, [this] { return !statsRunning_.load() || !pendingSamples_.empty(); });
            if (!statsRunning_.load() && pendingSamples_.empty()) break;
            drained.swap(pendingSamples_);
        }

        if (!drained.empty()) {
            std::scoped_lock ringLock(ringRatioMutex_);
            for (const auto& s : drained) {
                ringRatioBuffer_.push_back(s.ringRatio);
                ringRatioTimestamps_.push_back(s.timestampNs);
                if (ringRatioBuffer_.size() > MAX_BUFFER_SIZE) {
                    ringRatioBuffer_.pop_front();
                    ringRatioTimestamps_.pop_front();
                }
            }
            updateStatistics();
        }
        drained.clear();

        // Batch further incoming samples for kMinDrainInterval before the
        // next wake. Samples arriving during this sleep accumulate in
        // pendingSamples_ and are handled in one pass on the next iteration.
        std::this_thread::sleep_for(kMinDrainInterval);
    }

    SPDLOG_INFO("AutofocusService: Stats loop stopped");
}

void AutofocusService::controlLoop() {
    SPDLOG_INFO("AutofocusService: Control loop started");

    while (running_.load()) {
        if (!connected_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Handle manual voltage control requests
        {
            std::scoped_lock controlLock(controlMutex_);
            Config cfg;
            {
                std::scoped_lock cfgLock(configMutex_);
                cfg = config_;
            }

            if (increaseVoltageRequest_.exchange(false)) {
                double newVoltage =
                    std::min(currentVoltage_.load() + cfg.manualVoltageStep, cfg.maxVoltage);
                std::string error;
                if (writeDeviceVoltage(newVoltage, error)) {
                    SPDLOG_DEBUG("AutofocusService: Manual voltage increased to {}V", newVoltage);
                    notifyStatus("Voltage: " + std::to_string(newVoltage) + "V");
                } else {
                    SPDLOG_ERROR("AutofocusService: Manual voltage increase failed: {}", error);
                    notifyStatus("Voltage change failed: " + error);
                }
            }

            if (decreaseVoltageRequest_.exchange(false)) {
                double newVoltage =
                    std::max(currentVoltage_.load() - cfg.manualVoltageStep, cfg.minVoltage);
                std::string error;
                if (writeDeviceVoltage(newVoltage, error)) {
                    SPDLOG_DEBUG("AutofocusService: Manual voltage decreased to {}V", newVoltage);
                    notifyStatus("Voltage: " + std::to_string(newVoltage) + "V");
                } else {
                    SPDLOG_ERROR("AutofocusService: Manual voltage decrease failed: {}", error);
                    notifyStatus("Voltage change failed: " + error);
                }
            }
        }

        // Run automatic control if enabled
        if (enabled_.load() && connected_.load()) {
            Config cfg;
            {
                std::scoped_lock cfgLock(configMutex_);
                cfg = config_;
            }

            // Update current voltage from device
            double currentVolt = 0.0;
            std::string readError;
            if (readDeviceVoltage(currentVolt, readError)) {
                currentVoltage_.store(currentVolt);
            } else {
                SPDLOG_WARN("AutofocusService: Voltage read failed: {}", readError);
            }

            // Get median ring ratio
            double medianRingRatio = medianRingRatio_.load();

            // Check freshness and sample requirements (monotonic clock for consistent staleness)
            uint64_t currentSequence = ringRatioSequence_.load(std::memory_order_relaxed);
            uint64_t lastUpdateUs = lastRingRatioUpdateUs_.load(std::memory_order_relaxed);
            uint64_t nowUs = backend::Tools::getTimestamp();
            bool freshTimestamp =
                (lastUpdateUs > 0) &&
                (nowUs - lastUpdateUs <= static_cast<uint64_t>(cfg.ringRatioStaleMs) * 1000ULL);
            bool hasNewSample = (currentSequence != lastAppliedSequence_);
            uint64_t samplesSinceStep = currentSequence - lastAppliedSequence_;
            bool hasEnoughSamples =
                samplesSinceStep >= static_cast<uint64_t>(cfg.minSamplesPerStep);

            // Only perform autofocus control if we have valid data
            if (medianRingRatio > 0.0 && freshTimestamp &&
                (!cfg.requireNewSamplePerStep || hasNewSample) && hasEnoughSamples) {

                const double newVoltage = autofocus::computeFocusVoltage(
                    medianRingRatio, currentVoltage_.load(),
                    autofocus::FocusParams{cfg.focusSetpoint, cfg.focusRange, cfg.voltageStep,
                                           cfg.fineVoltageStep, cfg.minVoltage, cfg.maxVoltage,
                                           cfg.focusDirection});

                // Apply the new voltage if it changed
                if (std::abs(newVoltage - currentVoltage_.load()) > 0.01) {
                    std::string error;
                    if (!writeDeviceVoltage(newVoltage, error)) {
                        SPDLOG_ERROR("AutofocusService: Autofocus voltage step failed: {}", error);
                        notifyStatus("Autofocus voltage step failed: " + error);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }
                    lastAppliedSequence_ = currentSequence;

                    // Clear buffer after a step to ensure next statistics are based on post-step
                    // samples. Also drop anything queued in pendingSamples_ that hasn't yet been
                    // absorbed by statsLoop, otherwise pre-step samples would leak into the
                    // post-step buffer.
                    {
                        std::scoped_lock lock(pendingSamplesMutex_, ringRatioMutex_);
                        pendingSamples_.clear();
                        ringRatioBuffer_.clear();
                        ringRatioTimestamps_.clear();
                        ringRatioSequence_.store(0);
                        lastRingRatioTimestampNs_.store(0);
                        lastRingRatioUpdateUs_.store(0, std::memory_order_relaxed);
                        updateStatistics(); // Update statistics to reflect empty buffer
                    }

                    SPDLOG_DEBUG("AutofocusService: Adjusted voltage to {}V (ring width: {:.3f}, "
                                 "deviation: {:.3f})",
                                 newVoltage, medianRingRatio, medianRingRatio - cfg.focusSetpoint);
                    notifyStatus("Voltage: " + std::to_string(newVoltage) +
                                 "V (ring width: " + std::to_string(medianRingRatio) + ")");
                }
            }
        }

        // Sleep briefly to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SPDLOG_INFO("AutofocusService: Control loop stopped");
}

} // namespace backend::services
