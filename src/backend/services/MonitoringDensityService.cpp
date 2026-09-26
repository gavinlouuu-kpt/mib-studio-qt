#include "backend/services/MonitoringDensityService.h"

#include "backend/processing/KdeCoreRecord.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace backend::services {

namespace {

// The density estimate is presentation, never acquisition: under contention
// the OS must always prefer capture, processing, recording and the trigger.
// Best-effort; the result is reported in the stats.
bool lowerCurrentThreadPriority() {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST) != 0;
#else
    // SCHED_IDLE runs only when a core would otherwise idle; lowering needs
    // no privilege.
    sched_param sp{};
    sp.sched_priority = 0;
    return pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0;
#endif
}

double finiteOr(double v, double fallback) {
    return std::isfinite(v) ? v : fallback;
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

MonitoringDensitySettings MonitoringDensitySettings::clamped() const {
    const MonitoringDensitySettings d{};
    MonitoringDensitySettings s = *this;
    s.intervalMs = std::clamp(intervalMs, kIntervalMsMin, kIntervalMsMax);
    s.bandwidthFactor = std::clamp(finiteOr(bandwidthFactor, d.bandwidthFactor),
                                   kBandwidthFactorMin, kBandwidthFactorMax);
    s.coreFraction =
        std::clamp(finiteOr(coreFraction, d.coreFraction), kCoreFractionMin, kCoreFractionMax);
    if (!(std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1))) {
        s.x0 = s.x1 = s.y0 = s.y1 = 0.0;
    }
    return s;
}

bool MonitoringDensitySettings::operator==(const MonitoringDensitySettings& o) const {
    return enabled == o.enabled && intervalMs == o.intervalMs &&
           bandwidthFactor == o.bandwidthFactor && coreFraction == o.coreFraction && x0 == o.x0 &&
           x1 == o.x1 && y0 == o.y0 && y1 == o.y1;
}

bool MonitoringDensityService::Fingerprint::operator==(const Fingerprint& o) const {
    // Only what changes the estimate: the interval and the enabled flag do not.
    return count == o.count && first == o.first && last == o.last &&
           pixelToMicron == o.pixelToMicron &&
           settings.bandwidthFactor == o.settings.bandwidthFactor &&
           settings.coreFraction == o.settings.coreFraction && settings.x0 == o.settings.x0 &&
           settings.x1 == o.settings.x1 && settings.y0 == o.settings.y0 &&
           settings.y1 == o.settings.y1;
}

MonitoringDensityService::MonitoringDensityService(InputProvider input, LoadProbe load,
                                                   RecordSink sink)
    : input_(std::move(input)), load_(std::move(load)), sink_(std::move(sink)) {
    worker_ = std::thread([this] { run(); });
}

MonitoringDensityService::~MonitoringDensityService() {
    stop();
}

void MonitoringDensityService::stop() {
    {
        std::scoped_lock lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MonitoringDensityService::setSettings(const MonitoringDensitySettings& settings) {
    const MonitoringDensitySettings next = settings.clamped();
    {
        std::scoped_lock lk(mutex_);
        if (next == settings_) return;
        const bool disabling = settings_.enabled && !next.enabled;
        settings_ = next;
        if (disabling) {
            latest_.reset();
            haveFingerprint_ = false;
            generation_.fetch_add(1, std::memory_order_acq_rel);
        }
        wake_ = next.enabled;
    }
    cv_.notify_all();
}

MonitoringDensitySettings MonitoringDensityService::settings() const {
    std::scoped_lock lk(mutex_);
    return settings_;
}

void MonitoringDensityService::requestUpdate() {
    {
        std::scoped_lock lk(mutex_);
        if (!settings_.enabled) return;
        wake_ = true;
    }
    cv_.notify_all();
}

std::shared_ptr<const MonitoringDensityResult> MonitoringDensityService::latest() const {
    std::scoped_lock lk(mutex_);
    return latest_;
}

bool MonitoringDensityService::busy() const {
    std::scoped_lock lk(mutex_);
    return wake_ || computing_;
}

MonitoringDensityStats MonitoringDensityService::stats() const {
    std::scoped_lock lk(mutex_);
    return stats_;
}

int MonitoringDensityService::nextIntervalMs(int configuredMs, int lastComputeMs) {
    const int configured = std::clamp(configuredMs, MonitoringDensitySettings::kIntervalMsMin,
                                      MonitoringDensitySettings::kIntervalMsMax);
    const long long budget =
        static_cast<long long>(std::max(lastComputeMs, 0)) * kComputeBudgetFactor;
    return static_cast<int>(std::max<long long>(configured, budget));
}

bool MonitoringDensityService::underLoad(const MonitoringPipelineLoad& now,
                                         const MonitoringPipelineLoad& before) {
    if (now.droppedFrames > before.droppedFrames) return true;
    return now.queueCapacity > 0 && now.queueDepth * 4 >= now.queueCapacity;
}

MonitoringDensityResult
MonitoringDensityService::compute(const MonitoringDensityInput& input,
                                  const MonitoringDensitySettings& settings) {
    const auto t0 = std::chrono::steady_clock::now();
    MonitoringDensityResult r;
    r.frameIndices = input.frameIndices;
    r.bandwidthFactor = settings.bandwidthFactor;
    r.coreFraction = settings.coreFraction;
    r.pixelToMicron = input.pixelToMicron;
    r.gridNx = kGridNx;
    r.gridNy = kGridNy;
    const auto& points = input.points;
    r.bandwidth = monitoring::silvermanBandwidth(points, settings.bandwidthFactor);
    r.density = monitoring::gaussianKdeAtPoints(points, r.bandwidth);
    if (settings.x1 > settings.x0 && settings.y1 > settings.y0) {
        r.x0 = settings.x0;
        r.x1 = settings.x1;
        r.y0 = settings.y0;
        r.y1 = settings.y1;
    } else if (!points.empty()) {
        double x0 = points.front().x, x1 = x0, y0 = points.front().y, y1 = y0;
        for (const auto& p : points) {
            if (!monitoring::isFinitePoint(p)) continue;
            x0 = std::min(x0, p.x);
            x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y);
            y1 = std::max(y1, p.y);
        }
        const double padX = x1 > x0 ? 0.1 * (x1 - x0) : 1.0;
        const double padY = y1 > y0 ? 0.1 * (y1 - y0) : 0.01;
        r.x0 = x0 - padX;
        r.x1 = x1 + padX;
        r.y0 = y0 - padY;
        r.y1 = y1 + padY;
    }
    r.coreLevel = monitoring::coreLevel(r.density, settings.coreFraction);
    if (std::isfinite(r.coreLevel)) {
        for (double d : r.density)
            if (d >= r.coreLevel) ++r.coreCount;
        const auto grid = monitoring::gaussianKdeGrid(
            points, r.bandwidth, monitoring::rawKdeMaximum(points, r.bandwidth), r.x0, r.x1, r.y0,
            r.y1, kGridNx, kGridNy);
        r.contours = monitoring::isoContours(grid, r.coreLevel);
    }
    r.computeUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    r.computeMs = static_cast<int>((r.computeUs + 999) / 1000);
    r.computedAtNs = nowNs();
    return r;
}

std::string MonitoringDensityService::liveRecordJson(const MonitoringDensityResult& result) {
    monitoring::KdeCoreRecord r;
    r.provisional = true;
    r.source = "live-buffer";
    r.coreFraction = result.coreFraction;
    r.level = result.coreLevel;
    r.cellCount = result.coreCount;
    r.populationCount = result.frameIndices.size();
    r.bandwidthFactor = result.bandwidthFactor;
    r.bandwidthX = result.bandwidth.x;
    r.bandwidthY = result.bandwidth.y;
    r.pixelToMicron = result.pixelToMicron;
    r.x0 = result.x0;
    r.x1 = result.x1;
    r.y0 = result.y0;
    r.y1 = result.y1;
    r.gridNx = result.gridNx;
    r.gridNy = result.gridNy;
    r.contours = result.contours;
    r.computedAtNs = result.computedAtNs;
    return monitoring::toJson(r);
}

void MonitoringDensityService::run() {
    const bool lowered = lowerCurrentThreadPriority();
    {
        std::scoped_lock lk(mutex_);
        stats_.priorityLowered = lowered;
    }
    if (!lowered) SPDLOG_WARN("MonitoringDensityService: could not lower the worker priority");
    std::unique_lock lk(mutex_);
    while (!stop_) {
        if (!settings_.enabled) {
            cv_.wait(lk, [this] { return stop_ || (settings_.enabled && wake_); });
            continue;
        }
        const int waitMs = nextIntervalMs(settings_.intervalMs, stats_.lastComputeMs);
        stats_.nextIntervalMs = waitMs;
        cv_.wait_for(lk, std::chrono::milliseconds(waitMs), [this] { return stop_ || wake_; });
        if (stop_) break;
        if (!settings_.enabled) continue;
        wake_ = false;
        computing_ = true;
        lk.unlock();
        tick();
        lk.lock();
        computing_ = false;
    }
}

void MonitoringDensityService::tick() {
    MonitoringDensitySettings settings;
    {
        std::scoped_lock lk(mutex_);
        settings = settings_;
    }
    if (load_) {
        const MonitoringPipelineLoad now = load_();
        std::scoped_lock lk(mutex_);
        const bool loaded = underLoad(now, haveLastLoad_ ? lastLoad_ : now);
        lastLoad_ = now;
        haveLastLoad_ = true;
        if (loaded) {
            ++stats_.skippedUnderLoad;
            SPDLOG_DEBUG("MonitoringDensityService: pipeline under load (dropped {}, queue {}/{}), "
                         "estimate skipped",
                         now.droppedFrames, now.queueDepth, now.queueCapacity);
            return;
        }
    }
    MonitoringDensityInput input = input_ ? input_() : MonitoringDensityInput{};
    Fingerprint fp;
    fp.count = input.points.size();
    fp.first = input.frameIndices.empty() ? 0 : input.frameIndices.front();
    fp.last = input.frameIndices.empty() ? 0 : input.frameIndices.back();
    fp.pixelToMicron = input.pixelToMicron;
    fp.settings = settings;
    {
        std::scoped_lock lk(mutex_);
        if (haveFingerprint_ && fp == fingerprint_) {
            ++stats_.skippedUnchanged;
            return;
        }
        if (input.points.empty()) {
            // Nothing to estimate: drop a stale result once.
            fingerprint_ = fp;
            haveFingerprint_ = true;
            if (latest_) {
                latest_.reset();
                generation_.fetch_add(1, std::memory_order_acq_rel);
            }
            return;
        }
    }
    auto result = std::make_shared<MonitoringDensityResult>(compute(input, settings));
    std::string json;
    {
        std::scoped_lock lk(mutex_);
        stats_.busyMs += static_cast<double>(result->computeUs) / 1000.0;
        stats_.lastComputeMs = result->computeMs;
        ++stats_.estimates;
        if (!settings_.enabled) return; // switched off while computing
        fingerprint_ = fp;
        haveFingerprint_ = true;
        result->generation = generation_.load(std::memory_order_relaxed) + 1;
        latest_ = result;
        generation_.store(result->generation, std::memory_order_release);
        if (sink_) json = liveRecordJson(*result);
    }
    if (!json.empty()) sink_(std::move(json));
    SPDLOG_DEBUG("MonitoringDensityService: {} points, core {:.0f}% -> {} cells, {} loop(s), {} ms",
                 result->frameIndices.size(), result->coreFraction * 100.0, result->coreCount,
                 result->contours.size(), result->computeMs);
}

} // namespace backend::services
