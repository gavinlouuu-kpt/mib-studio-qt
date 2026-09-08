#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#if defined(__has_include)
#  if __has_include(<nlohmann/json_fwd.hpp>)
#    include <nlohmann/json_fwd.hpp>
#    define MIB_HAS_NLOHMANN_JSON 1
#  endif
#endif
#ifndef MIB_HAS_NLOHMANN_JSON
#  define MIB_HAS_NLOHMANN_JSON 0
#endif

namespace backend::diagnostics {

// Lock-free state mirror that services write atomic snapshots into.
// The crash handler reads from this without taking any locks (paths use a
// short try_lock and are skipped on contention) so it is safe to call from
// signal/SEH context.
//
// One global instance accessed via CrashStateMirror::instance(). Each service
// area has its own sub-struct so writes do not contend with each other.
class CrashStateMirror {
public:
    struct CaptureSlot {
        std::atomic<bool> running{false};
        std::atomic<uint64_t> framesProcessed{0};
        std::atomic<uint64_t> lastFrameRate{0};
        std::atomic<uint64_t> lastDataRateMBps{0};
        std::atomic<int> bufferPartCount{0};
        std::atomic<int> numBuffers{0};
    };

    struct ProcessingSlot {
        std::atomic<bool> running{false};
        std::atomic<bool> realtimeRunning{false};
        std::atomic<bool> experimentActive{false};
        std::atomic<int>  workerCount{0};
        std::atomic<uint64_t> jobsQueued{0};
        std::atomic<uint64_t> jobsProcessed{0};
        std::atomic<double> algoFps{0.0};
        std::atomic<double> validFps{0.0};
        std::atomic<double> invalidFps{0.0};
        std::atomic<uint64_t> totalValidFlushed{0};
        // Target-identification loss at crash time (see IdentificationCounters
        // / getDroppedValidFrames): real detections dropped at the backlog cap,
        // total sort targets identified, and targets that never got a pulse
        // because a frame produced more than one.
        std::atomic<uint64_t> droppedValidFrames{0};
        std::atomic<uint64_t> targetGroupObjects{0};
        std::atomic<uint64_t> unservedTargetGroupObjects{0};
    };

    struct Hdf5Slot {
        std::atomic<bool> fileOpen{false};
        std::atomic<uint64_t> pendingValid{0};
        std::atomic<uint64_t> pendingInvalid{0};
        std::atomic<uint64_t> appendedValid{0};
        std::atomic<uint64_t> appendedInvalid{0};
        static constexpr size_t kPathMax = 512;
        mutable std::mutex pathMutex;
        char path[kPathMax]{};
    };

    struct FrameStoreSlot {
        std::atomic<size_t>   capacity{0};
        std::atomic<uint64_t> totalWritten{0};
        std::atomic<uint64_t> totalFiltered{0};
        std::atomic<uint64_t> earliestIndex{0};
        std::atomic<uint64_t> latestIndex{0};
    };

    struct AutofocusSlot {
        std::atomic<bool> connected{false};
        std::atomic<bool> enabled{false};
        std::atomic<double> voltage{0.0};
        std::atomic<double> ringRatioAvg{0.0};
        std::atomic<double> ringRatioMedian{0.0};
        std::atomic<uint64_t> lastUpdateUs{0};
        static constexpr size_t kPortMax = 32;
        mutable std::mutex portMutex;
        char comPort[kPortMax]{};
    };

    struct PumpSlot {
        std::atomic<bool> connected{false};
        std::atomic<int>  runStatus{-1};
        std::atomic<double> flowRate{0.0};
        std::atomic<double> volume{0.0};
        std::atomic<bool> stalled{false};
    };

    struct SyringePumpSlot {
        PumpSlot sample;
        PumpSlot sheath;
    };

    struct TriggerSlot {
        std::atomic<bool> running{false};
        std::atomic<uint64_t> triggerCount{0};
        std::atomic<uint64_t> lastOnsetUs{0};
        // Sort losses at crash time: requests evicted from a full queue, and
        // pulses that could not be driven after dequeue (no camera / set
        // failed). Plus the live acquisition->pulse latency EWMA.
        std::atomic<uint64_t> droppedRequests{0};
        std::atomic<uint64_t> droppedPulses{0};
        std::atomic<uint64_t> targetLatencyUs{0};
    };

    struct RecorderSlot {
        std::atomic<bool> recording{false};
        std::atomic<uint64_t> framesWritten{0};
        std::atomic<uint64_t> framesFiltered{0};
    };

    struct AppSlot {
        std::atomic<int> activeTabIndex{-1};
        std::atomic<bool> mockCamera{false};
        std::atomic<int> selectedInterface{-1};
        std::atomic<int> selectedDevice{-1};
        static constexpr size_t kLabelMax = 128;
        mutable std::mutex labelMutex;
        char cameraLabel[kLabelMax]{};
        char dataDir[256]{};
        char buildVersion[64]{};
    };

    static CrashStateMirror& instance();

    CaptureSlot       capture;
    ProcessingSlot    processing;
    Hdf5Slot          hdf5;
    FrameStoreSlot    frameStore;
    AutofocusSlot     autofocus;
    SyringePumpSlot   syringePump;
    TriggerSlot       trigger;
    RecorderSlot      recorder;
    AppSlot           app;

    // Convenience helpers for string fields (write into the mutex-protected
    // fixed-size buffers). Truncates silently when too long.
    void setHdf5Path(const std::string& path);
    void clearHdf5Path();
    void setAutofocusPort(const std::string& port);
    void setCameraLabel(const std::string& label);
    void setDataDir(const std::string& dir);
    void setBuildVersion(const std::string& version);

    // Serialize the current state to a JSON string. Safe to call from a
    // crash handler: only uses atomic reads and try_lock for string fields.
    std::string snapshotJsonString() const;

private:
    CrashStateMirror() = default;
    CrashStateMirror(const CrashStateMirror&) = delete;
    CrashStateMirror& operator=(const CrashStateMirror&) = delete;
};

} // namespace backend::diagnostics
