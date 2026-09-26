#pragma once
// MonitoringDensityService: the live Monitoring scatter density (KDE) and its
// core contour, computed in the backend so every shell (Qt today, React/Tauri
// next) reads the same result and the estimate can never compete with the
// acquisition pipeline.
//
// One worker thread at the lowest OS priority (SCHED_IDLE on Linux,
// THREAD_PRIORITY_LOWEST on Windows) wakes every `intervalMs`, copies the
// valid monitoring cells (index, area, deformability only) through an input
// provider, and runs the Qt-free kernel in backend/processing/MonitoringDensity.h.
// Guards, in the order they are checked on each tick:
//   - disabled: nothing runs;
//   - pipeline under load (dropped frames since the last tick, or the batch
//     queue at least a quarter full): the tick is skipped and counted;
//   - unchanged input and settings: skipped (the ring only grows at the back
//     and trims at the front, so count + first + last index identify it);
//   - the next wake is at least `kComputeBudgetFactor` x the last compute's
//     CPU time, so the worker's duty cycle stays <= ~5% of one core on any
//     host (CPU, not wall: a starved worker must not stretch its own budget).
// After each estimate the provisional core record (backend/processing/
// KdeCoreRecord.h) goes to the record sink; AppBackend wires it to
// ExperimentCoordinator::setLiveKdeCoreRecord, which keeps it only while a
// run is Active and writes it at finalization.

#include "backend/processing/MonitoringDensity.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backend::services {

struct MonitoringDensitySettings {
    static constexpr int kIntervalMsMin = 500;
    static constexpr int kIntervalMsMax = 60000;
    static constexpr double kBandwidthFactorMin = 0.2;
    static constexpr double kBandwidthFactorMax = 5.0;
    static constexpr double kCoreFractionMin = 0.05;
    static constexpr double kCoreFractionMax = 1.0;

    bool enabled{false};
    int intervalMs{2000};
    double bandwidthFactor{1.0};
    double coreFraction{0.9};
    // Range the core contour grid spans (µm² x deformability): the shell's
    // chart axes. Empty (x1 <= x0 or y1 <= y0): the data range padded by 10%.
    double x0{0.0}, x1{0.0}, y0{0.0}, y1{0.0};

    // Clamped copy; non-finite values fall back to the defaults.
    MonitoringDensitySettings clamped() const;
    bool operator==(const MonitoringDensitySettings& o) const;
    bool operator!=(const MonitoringDensitySettings& o) const { return !(*this == o); }
};

// Valid monitoring cells, oldest -> newest, already in chart units.
struct MonitoringDensityInput {
    std::vector<std::uint64_t> frameIndices;
    std::vector<monitoring::DensityPoint> points; // parallel to frameIndices
    double pixelToMicron{0.0};
};

// Acquisition-pipeline pressure, sampled once per tick.
struct MonitoringPipelineLoad {
    std::uint64_t droppedFrames{0}; // monotonic
    std::size_t queueDepth{0};
    std::size_t queueCapacity{0}; // 0 = unknown / no queue
};

struct MonitoringDensityResult {
    std::uint64_t generation{0};
    std::vector<std::uint64_t> frameIndices;
    std::vector<double> density; // normalised [0, 1], parallel to frameIndices
    monitoring::DensityBandwidth bandwidth{};
    double bandwidthFactor{0.0};
    double coreFraction{0.0};
    double coreLevel{0.0}; // NaN below three cells
    std::size_t coreCount{0};
    std::vector<monitoring::Contour> contours; // µm² x deformability
    double pixelToMicron{0.0};
    double x0{0.0}, x1{0.0}, y0{0.0}, y1{0.0}; // grid range used
    int gridNx{0}, gridNy{0};
    // CPU time of the estimate on the computing thread (what the budget
    // charges; a starved lowest-priority worker waits without consuming CPU),
    // and the wall time it took, for diagnostics.
    int computeMs{0}; // CPU, rounded up
    std::int64_t computeUs{0};
    int wallMs{0};
    std::uint64_t computedAtNs{0}; // system clock
};

struct MonitoringDensityStats {
    std::uint64_t estimates{0};
    std::uint64_t skippedUnchanged{0};
    std::uint64_t skippedUnderLoad{0};
    double busyMs{0.0};   // total CPU time spent computing estimates
    int lastComputeMs{0}; // CPU
    int lastWallMs{0};
    int nextIntervalMs{0};
    bool priorityLowered{false};
};

class MonitoringDensityService {
public:
    static constexpr int kGridNx = 128;
    static constexpr int kGridNy = 64;
    static constexpr int kComputeBudgetFactor = 20;

    using InputProvider = std::function<MonitoringDensityInput()>;
    using LoadProbe = std::function<MonitoringPipelineLoad()>;
    using RecordSink = std::function<void(std::string json)>;

    // Starts the (idle) worker. The callbacks run on the worker thread and
    // must stay valid until stop(); the probe and sink may be empty.
    explicit MonitoringDensityService(InputProvider input, LoadProbe load = {},
                                      RecordSink sink = {});
    ~MonitoringDensityService();
    MonitoringDensityService(const MonitoringDensityService&) = delete;
    MonitoringDensityService& operator=(const MonitoringDensityService&) = delete;

    // Clamps and applies; a change wakes the worker. Disabling drops the last
    // result, so re-enabling recomputes even for unchanged input.
    void setSettings(const MonitoringDensitySettings& settings);
    MonitoringDensitySettings settings() const;
    // Wake the worker now (a no-op while disabled).
    void requestUpdate();
    // Last completed estimate (null before the first or while disabled).
    std::shared_ptr<const MonitoringDensityResult> latest() const;
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    // A requested tick has not finished yet (pending or computing).
    bool busy() const;
    MonitoringDensityStats stats() const;
    // Join the worker; idempotent. Called by the destructor.
    void stop();

    // Pure policy, exposed for tests.
    static int nextIntervalMs(int configuredMs, int lastComputeMs);
    static bool underLoad(const MonitoringPipelineLoad& now, const MonitoringPipelineLoad& before);
    static MonitoringDensityResult compute(const MonitoringDensityInput& input,
                                           const MonitoringDensitySettings& settings);
    // Provisional core record JSON (schema: backend/processing/KdeCoreRecord.h).
    static std::string liveRecordJson(const MonitoringDensityResult& result);

private:
    struct Fingerprint {
        std::size_t count{0};
        std::uint64_t first{0}, last{0};
        double pixelToMicron{0.0};
        MonitoringDensitySettings settings{};
        bool operator==(const Fingerprint& o) const;
    };

    void run();
    void tick();

    InputProvider input_;
    LoadProbe load_;
    RecordSink sink_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    MonitoringDensitySettings settings_{};
    std::shared_ptr<const MonitoringDensityResult> latest_;
    bool stop_{false};
    bool wake_{false};
    bool computing_{false};
    bool haveFingerprint_{false};
    Fingerprint fingerprint_{};
    MonitoringPipelineLoad lastLoad_{};
    bool haveLastLoad_{false};
    MonitoringDensityStats stats_{};
    std::atomic<std::uint64_t> generation_{0};
    std::thread worker_;
};

} // namespace backend::services
