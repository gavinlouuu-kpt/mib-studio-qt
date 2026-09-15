#pragma once

#include "backend/nanopositioner/INanopositionerBackend.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backend::services {

class AutofocusService {
public:
    using BackendFactory =
        std::function<std::unique_ptr<backend::nanopositioner::INanopositionerBackend>(
            backend::nanopositioner::BackendKind)>;

    AutofocusService();
    explicit AutofocusService(BackendFactory backendFactory);
    ~AutofocusService();

    // Connection management
    bool connect(const backend::nanopositioner::Endpoint& endpoint);
    bool connect(int comPort, int baudRate, unsigned char deviceAddress);
    void disconnect();
    bool isConnected() const { return connected_.load(); }
    int getComPort() const { return comPort_; }
    backend::nanopositioner::BackendKind getBackendKind() const;
    std::string getEndpointId() const;

    static std::vector<backend::nanopositioner::Endpoint> availableEndpoints();
    static bool probeEndpoint(const backend::nanopositioner::Endpoint& endpoint);

    // Legacy CoreMOR COM wrapper. New code should use probeEndpoint/connect(Endpoint).
    static bool probeComPort(int comPort, int baudRate, unsigned char deviceAddress);

    // Autofocus control
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_.load(); }

    // Manual voltage control
    void increaseVoltage();
    void decreaseVoltage();
    double getCurrentVoltage() const { return currentVoltage_.load(); }

    // Configuration
    struct Config {
        double focusSetpoint{20.0};
        double focusRange{0.5};
        double voltageStep{1.0};
        double fineVoltageStep{0.2};
        double maxVoltage{100.0};
        double minVoltage{0.0};
        double initialVoltage{50.0};
        double manualVoltageStep{1.0};
        int ringRatioStaleMs{1500};
        bool requireNewSamplePerStep{true};
        int minSamplesPerStep{100};
        double safeShutdownVoltage{0.0};
        bool focusDirection{true}; // true = increase voltage increases ring ratio
    };

    void setConfig(const Config& config);
    Config getConfig() const;

    // Ring ratio feed (called by ProcessingService)
    void onRingRatio(double ringRatio, int64_t timestampNs);

    // Expose running average of ring ratio for UI/status
    double getAverageRingRatio() const { return averageRingRatio_.load(std::memory_order_relaxed); }
    // Expose median ring ratio (same value used by autofocus control) for UI/status
    double getMedianRingRatio() const { return medianRingRatio_.load(std::memory_order_relaxed); }
    // Monotonic timestamp (microseconds) when ring ratio was last updated; 0 if never
    uint64_t getLastRingRatioUpdateUs() const {
        return lastRingRatioUpdateUs_.load(std::memory_order_relaxed);
    }

    // Status callbacks for UI
    using StatusCallback = std::function<void(const std::string& message)>;
    void setStatusCallback(StatusCallback callback);

private:
    void controlLoop();
    void statsLoop();
    void updateStatistics();
    double calculateMedian(const std::vector<double>& sorted) const;
    void notifyStatus(const std::string& message) const;
    bool readDeviceVoltage(double& voltage, std::string& error);
    bool writeDeviceVoltage(double voltage, std::string& error);

    std::thread controlThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<double> currentVoltage_{0.0};

    // Connection parameters
    int comPort_{6};
    int baudRate_{115200};
    unsigned char deviceAddress_{1};
    mutable std::mutex deviceMutex_;
    std::unique_ptr<backend::nanopositioner::INanopositionerBackend> device_;
    BackendFactory backendFactory_;
    backend::nanopositioner::Endpoint endpoint_;
    std::atomic<bool> activeControlSession_{false};

    // Configuration
    mutable std::mutex configMutex_;
    Config config_;

    // Incoming samples (producer: ProcessingService realtime thread via
    // onRingRatio; consumer: statsThread_). onRingRatio does NOT touch the
    // ring-ratio buffer / stats / sort — that work is deferred to statsLoop
    // so the realtime thread only pays an atomic update + one push_back +
    // notify_one per valid frame.
    struct PendingSample {
        double ringRatio{0.0};
        int64_t timestampNs{0};
    };
    mutable std::mutex pendingSamplesMutex_;
    std::condition_variable pendingSamplesCV_;
    std::vector<PendingSample> pendingSamples_;
    std::thread statsThread_;
    std::atomic<bool> statsRunning_{false};

    // Ring ratio buffer (owned by statsLoop; read by controlLoop under
    // ringRatioMutex_; never touched from the realtime thread).
    mutable std::mutex ringRatioMutex_;
    std::deque<double> ringRatioBuffer_;
    std::deque<int64_t> ringRatioTimestamps_;
    static constexpr size_t MAX_BUFFER_SIZE = 1000;
    std::atomic<uint64_t> ringRatioSequence_{0};
    std::atomic<int64_t> lastRingRatioTimestampNs_{0};
    std::atomic<uint64_t> lastRingRatioUpdateUs_{
        0}; // monotonic us when a ring ratio sample was last accepted

    // Statistics
    std::atomic<double> medianRingRatio_{0.0};
    std::atomic<double> averageRingRatio_{0.0};
    std::atomic<double> minRingRatio_{0.0};
    std::atomic<double> maxRingRatio_{0.0};

    // Manual control requests
    std::atomic<bool> increaseVoltageRequest_{false};
    std::atomic<bool> decreaseVoltageRequest_{false};
    std::mutex controlMutex_;

    // Status callback
    mutable std::mutex callbackMutex_;
    StatusCallback statusCallback_;

    // Last applied sequence for freshness tracking
    uint64_t lastAppliedSequence_{0};
};

} // namespace backend::services
