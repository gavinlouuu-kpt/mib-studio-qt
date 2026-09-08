#include "backend/services/AutofocusService.h"
#include "backend/services/AutofocusMath.h"
#include "backend/app/Tools.h"
#include "backend/diagnostics/CrashStateMirror.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <Coremor/XMT_DLL_SER.h>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

namespace backend::services {

namespace {
    // Plausible voltage range for XMT nanopositioner (V). Used to validate probe response.
    // A CoreMorrow controller resting at 0 V reports ADC noise of about -1 mV
    // (-0.0004 .. -0.002 V measured on the bench), so the floor sits slightly
    // below zero; a genuinely bad response (timeout/garbage) is far outside.
    constexpr double PROBE_VOLTAGE_MIN = -0.05;
    constexpr double PROBE_VOLTAGE_MAX = 250.0;
    constexpr int SERIAL_OPEN_ATTEMPTS = 3;
    constexpr auto SERIAL_RETRY_DELAY = std::chrono::milliseconds(150);

    std::mutex& xmtSerialMutex() {
        static std::mutex mutex;
        return mutex;
    }

    bool isPlausibleProbeVoltage(double val) {
        return std::isfinite(val) && val >= PROBE_VOLTAGE_MIN && val <= PROBE_VOLTAGE_MAX;
    }

    int openComWithRetriesLocked(int comPort, int baudRate) {
        int result = 0;
        for (int attempt = 1; attempt <= SERIAL_OPEN_ATTEMPTS; ++attempt) {
            CloseSer();
            result = OpenComConnectRS232(comPort, baudRate);
            if (result != 0) {
                return result;
            }
            SPDLOG_DEBUG("AutofocusService: COM{} open attempt {}/{} failed",
                         comPort, attempt, SERIAL_OPEN_ATTEMPTS);
            std::this_thread::sleep_for(SERIAL_RETRY_DELAY);
        }
        return result;
    }
} // namespace

AutofocusService::AutofocusService() {
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

bool AutofocusService::connect(int comPort, int baudRate, unsigned char deviceAddress) {
    if (connected_.load()) {
        disconnect();
    }

    comPort_ = comPort;
    baudRate_ = baudRate;
    deviceAddress_ = deviceAddress;

    {
        std::scoped_lock serialLock(xmtSerialMutex());
        int result = openComWithRetriesLocked(comPort, baudRate);
        if (result == 0) {
            SPDLOG_ERROR("AutofocusService: Failed to open COM{} at {} baud after {} attempts",
                         comPort, baudRate, SERIAL_OPEN_ATTEMPTS);
            if (statusCallback_) {
                statusCallback_("Failed to open COM port " + std::to_string(comPort));
            }
            connected_.store(false);
            CloseSer();
            return false;
        }

        double currentVolt = XMT_COMMAND_ReadData(deviceAddress_, 5, 0, 0);
        if (isPlausibleProbeVoltage(currentVolt)) {
            currentVoltage_.store(currentVolt);
            SPDLOG_INFO("AutofocusService: COM{} opened and responded at {:.2f} V", comPort, currentVolt);
        } else {
            // Some CoreMOR controllers return an invalid first read immediately after open.
            // Keep the explicit user-selected connection open, but log the value for diagnostics
            // instead of treating a single bad read as a hard failure.
            SPDLOG_WARN("AutofocusService: COM{} opened but initial voltage read was not plausible: {}",
                        comPort, currentVolt);
        }

        connected_.store(true);
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance().autofocus;
            m.connected.store(true);
            m.voltage.store(currentVoltage_.load());
            backend::diagnostics::CrashStateMirror::instance().setAutofocusPort(
                "COM" + std::to_string(comPort));
        }

        // Initialize voltage
        {
            std::scoped_lock cfgLock(configMutex_);
            // Clamp to the configured safe range so a stale/misconfigured
            // initialVoltage can never drive the probe past its limits.
            const double initialVoltage = autofocus::clampVoltage(
                config_.initialVoltage, config_.minVoltage, config_.maxVoltage);
            XMT_COMMAND_SinglePoint(deviceAddress_, 0, 0, 0, initialVoltage);
            currentVoltage_.store(initialVoltage);
            backend::diagnostics::CrashStateMirror::instance().autofocus.voltage.store(initialVoltage);
        }
    }

    // Start control thread
    if (!running_.load()) {
        running_.store(true);
        controlThread_ = std::thread(&AutofocusService::controlLoop, this);
    }

    if (statusCallback_) {
        statusCallback_("Connected to nanopositioner on COM" + std::to_string(comPort));
    }

    return true;
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

    // Set safe shutdown voltage and close the global SDK handle under the serial mutex.
    {
        std::scoped_lock serialLock(xmtSerialMutex());
        std::scoped_lock cfgLock(configMutex_);
        double safeVoltage = config_.safeShutdownVoltage;
        if (connected_.load()) {
            XMT_COMMAND_SinglePoint(deviceAddress_, 0, 0, 0, safeVoltage);
        }
        CloseSer();
    }

    // Close COM port
    connected_.store(false);
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
    if (statusCallback_) {
        statusCallback_("Disconnected from nanopositioner");
    }
}

bool AutofocusService::probeComPort(int comPort, int baudRate, unsigned char deviceAddress) {
    // Caller must not be connected (SDK uses a single global COM handle).
    std::scoped_lock serialLock(xmtSerialMutex());
    int result = openComWithRetriesLocked(comPort, baudRate);
    if (result == 0) {
        CloseSer();
        SPDLOG_DEBUG("AutofocusService: COM{} probe failed to open at {} baud", comPort, baudRate);
        return false;
    }

    double val = XMT_COMMAND_ReadData(deviceAddress, 5, 0, 0);
    if (!isPlausibleProbeVoltage(val)) {
        std::this_thread::sleep_for(SERIAL_RETRY_DELAY);
        val = XMT_COMMAND_ReadData(deviceAddress, 5, 0, 0);
    }
    CloseSer();
    bool plausible = isPlausibleProbeVoltage(val);
    if (plausible) {
        SPDLOG_DEBUG("AutofocusService: COM{} probe OK (read {:.2f} V)", comPort, val);
    } else {
        SPDLOG_DEBUG("AutofocusService: COM{} probe rejected voltage response {}", comPort, val);
    }
    return plausible;
}

void AutofocusService::setEnabled(bool enabled) {
    enabled_.store(enabled);
    backend::diagnostics::CrashStateMirror::instance().autofocus.enabled.store(enabled);
    SPDLOG_INFO("AutofocusService: Autofocus {}", enabled ? "enabled" : "disabled");
    if (statusCallback_) {
        statusCallback_(enabled ? "Autofocus enabled" : "Autofocus disabled");
    }
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

    std::vector<double> sorted = std::vector<double>(ringRatioBuffer_.begin(), ringRatioBuffer_.end());
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
            pendingSamplesCV_.wait(lk, [this] {
                return !statsRunning_.load() || !pendingSamples_.empty();
            });
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

            if (increaseVoltageRequest_.load()) {
                double newVoltage = std::min(currentVoltage_.load() + cfg.manualVoltageStep, cfg.maxVoltage);
                {
                    std::scoped_lock serialLock(xmtSerialMutex());
                    XMT_COMMAND_SinglePoint(deviceAddress_, 0, 0, 0, newVoltage);
                }
                currentVoltage_.store(newVoltage);
                SPDLOG_DEBUG("AutofocusService: Manual voltage increased to {}V", newVoltage);
                increaseVoltageRequest_.store(false);
                if (statusCallback_) {
                    statusCallback_("Voltage: " + std::to_string(newVoltage) + "V");
                }
            }

            if (decreaseVoltageRequest_.load()) {
                double newVoltage = std::max(currentVoltage_.load() - cfg.manualVoltageStep, cfg.minVoltage);
                {
                    std::scoped_lock serialLock(xmtSerialMutex());
                    XMT_COMMAND_SinglePoint(deviceAddress_, 0, 0, 0, newVoltage);
                }
                currentVoltage_.store(newVoltage);
                SPDLOG_DEBUG("AutofocusService: Manual voltage decreased to {}V", newVoltage);
                decreaseVoltageRequest_.store(false);
                if (statusCallback_) {
                    statusCallback_("Voltage: " + std::to_string(newVoltage) + "V");
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
            {
                std::scoped_lock serialLock(xmtSerialMutex());
                currentVolt = XMT_COMMAND_ReadData(deviceAddress_, 5, 0, 0);
            }
            if (isPlausibleProbeVoltage(currentVolt)) {
                currentVoltage_.store(currentVolt);
            }

            // Get median ring ratio
            double medianRingRatio = medianRingRatio_.load();

            // Check freshness and sample requirements (monotonic clock for consistent staleness)
            uint64_t currentSequence = ringRatioSequence_.load(std::memory_order_relaxed);
            uint64_t lastUpdateUs = lastRingRatioUpdateUs_.load(std::memory_order_relaxed);
            uint64_t nowUs = backend::Tools::getTimestamp();
            bool freshTimestamp = (lastUpdateUs > 0) &&
                                  (nowUs - lastUpdateUs <= static_cast<uint64_t>(cfg.ringRatioStaleMs) * 1000ULL);
            bool hasNewSample = (currentSequence != lastAppliedSequence_);
            uint64_t samplesSinceStep = currentSequence - lastAppliedSequence_;
            bool hasEnoughSamples = samplesSinceStep >= static_cast<uint64_t>(cfg.minSamplesPerStep);

            // Only perform autofocus control if we have valid data
            if (medianRingRatio > 0.0 && freshTimestamp && 
                (!cfg.requireNewSamplePerStep || hasNewSample) && hasEnoughSamples) {
                
                const double newVoltage = autofocus::computeFocusVoltage(
                    medianRingRatio, currentVoltage_.load(),
                    autofocus::FocusParams{cfg.focusSetpoint, cfg.focusRange,
                                           cfg.voltageStep, cfg.fineVoltageStep,
                                           cfg.minVoltage, cfg.maxVoltage,
                                           cfg.focusDirection});

                // Apply the new voltage if it changed
                if (std::abs(newVoltage - currentVoltage_.load()) > 0.01) {
                    {
                        std::scoped_lock serialLock(xmtSerialMutex());
                        XMT_COMMAND_SinglePoint(deviceAddress_, 0, 0, 0, newVoltage);
                    }
                    currentVoltage_.store(newVoltage);
                    lastAppliedSequence_ = currentSequence;

                    // Clear buffer after a step to ensure next statistics are based on post-step samples.
                    // Also drop anything queued in pendingSamples_ that hasn't yet been absorbed by
                    // statsLoop, otherwise pre-step samples would leak into the post-step buffer.
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

                    SPDLOG_DEBUG("AutofocusService: Adjusted voltage to {}V (ring width: {:.3f}, deviation: {:.3f})",
                                newVoltage, medianRingRatio, medianRingRatio - cfg.focusSetpoint);
                    if (statusCallback_) {
                        statusCallback_("Voltage: " + std::to_string(newVoltage) + "V (ring width: " + 
                                      std::to_string(medianRingRatio) + ")");
                    }
                }
            }
        }

        // Sleep briefly to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SPDLOG_INFO("AutofocusService: Control loop stopped");
}

} // namespace backend::services

