#include "backend/services/AutofocusService.h"
#include "backend/app/Tools.h"

#include <spdlog/spdlog.h>

namespace backend::services {

namespace
{
    constexpr const char *kUnsupportedMsg =
        "AutofocusService is unavailable on this platform (Coremor SDK is Windows-only)";
}

AutofocusService::AutofocusService() = default;

AutofocusService::~AutofocusService() = default;

bool AutofocusService::connect(int comPort, int baudRate, unsigned char deviceAddress)
{
    (void)comPort;
    (void)baudRate;
    (void)deviceAddress;
    SPDLOG_WARN("{}", kUnsupportedMsg);
    notifyStatus(kUnsupportedMsg);
    connected_.store(false);
    return false;
}

void AutofocusService::disconnect()
{
    connected_.store(false);
    running_.store(false);
}

bool AutofocusService::probeComPort(int comPort, int baudRate, unsigned char deviceAddress)
{
    (void)comPort;
    (void)baudRate;
    (void)deviceAddress;
    return false;
}

void AutofocusService::setEnabled(bool enabled)
{
    enabled_.store(enabled);
    if (enabled)
    {
        SPDLOG_WARN("{}", kUnsupportedMsg);
    }
}

void AutofocusService::increaseVoltage() {}

void AutofocusService::decreaseVoltage() {}

void AutofocusService::setConfig(const Config &config)
{
    std::scoped_lock lock(configMutex_);
    config_ = config;
}

AutofocusService::Config AutofocusService::getConfig() const
{
    std::scoped_lock lock(configMutex_);
    return config_;
}

void AutofocusService::onRingRatio(double ringRatio, int64_t timestampNs)
{
    if (ringRatio <= 0.0)
    {
        return;
    }

    ringRatioSequence_.fetch_add(1, std::memory_order_relaxed);
    lastRingRatioTimestampNs_.store(timestampNs, std::memory_order_relaxed);
    lastRingRatioUpdateUs_.store(backend::Tools::getTimestamp(), std::memory_order_relaxed);
}

void AutofocusService::updateStatistics()
{
    medianRingRatio_.store(0.0);
    averageRingRatio_.store(0.0);
    minRingRatio_.store(0.0);
    maxRingRatio_.store(0.0);
}

double AutofocusService::calculateMedian(const std::vector<double> &sorted) const
{
    (void)sorted;
    return 0.0;
}

void AutofocusService::setStatusCallback(StatusCallback callback)
{
    std::scoped_lock lock(callbackMutex_);
    statusCallback_ = std::move(callback);
}

void AutofocusService::statsLoop() {}

void AutofocusService::controlLoop() {}

} // namespace backend::services
