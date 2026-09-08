#include "backend/diagnostics/CrashStateMirror.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#if defined(__has_include) && __has_include(<nlohmann/json.hpp>)
#  include <nlohmann/json.hpp>
#  define MIB_CRASH_HAS_JSON 1
#endif

namespace backend::diagnostics {

namespace {

void copyTruncated(char* dst, size_t cap, const std::string& src) {
    if (cap == 0) return;
    const size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// Best-effort read of a mutex-protected char buffer. If the mutex is
// contended (which a crash handler should not block on) we return the
// raw buffer copy without locking. This is acceptable because string
// fields are diagnostic and the worst case is a truncated/garbled value.
std::string readSafeString(std::mutex& m, const char* buf, size_t cap) {
    char copy[1024];
    const size_t n = std::min<size_t>(cap, sizeof(copy));
    if (m.try_lock()) {
        std::memcpy(copy, buf, n);
        copy[n - 1] = '\0';
        m.unlock();
    } else {
        std::memcpy(copy, buf, n);
        copy[n - 1] = '\0';
    }
    return std::string(copy);
}

uint64_t nowEpochMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count());
}

} // namespace

CrashStateMirror& CrashStateMirror::instance() {
    static CrashStateMirror s;
    return s;
}

void CrashStateMirror::setHdf5Path(const std::string& path) {
    std::scoped_lock lk(hdf5.pathMutex);
    copyTruncated(hdf5.path, Hdf5Slot::kPathMax, path);
}

void CrashStateMirror::clearHdf5Path() {
    std::scoped_lock lk(hdf5.pathMutex);
    hdf5.path[0] = '\0';
}

void CrashStateMirror::setAutofocusPort(const std::string& port) {
    std::scoped_lock lk(autofocus.portMutex);
    copyTruncated(autofocus.comPort, AutofocusSlot::kPortMax, port);
}

void CrashStateMirror::setCameraLabel(const std::string& label) {
    std::scoped_lock lk(app.labelMutex);
    copyTruncated(app.cameraLabel, AppSlot::kLabelMax, label);
}

void CrashStateMirror::setDataDir(const std::string& dir) {
    std::scoped_lock lk(app.labelMutex); // share the lock; cheap
    copyTruncated(app.dataDir, sizeof(app.dataDir), dir);
}

void CrashStateMirror::setBuildVersion(const std::string& v) {
    std::scoped_lock lk(app.labelMutex);
    copyTruncated(app.buildVersion, sizeof(app.buildVersion), v);
}

#ifdef MIB_CRASH_HAS_JSON
std::string CrashStateMirror::snapshotJsonString() const {
    using json = nlohmann::json;
    json j;

    j["timestamp_ms"] = nowEpochMs();

    j["app"] = {
        {"active_tab_index", app.activeTabIndex.load()},
        {"mock_camera", app.mockCamera.load()},
        {"selected_interface", app.selectedInterface.load()},
        {"selected_device", app.selectedDevice.load()},
        {"camera_label", readSafeString(app.labelMutex, app.cameraLabel, AppSlot::kLabelMax)},
        {"data_dir", readSafeString(app.labelMutex, app.dataDir, sizeof(app.dataDir))},
        {"build_version", readSafeString(app.labelMutex, app.buildVersion, sizeof(app.buildVersion))},
    };

    j["capture"] = {
        {"running", capture.running.load()},
        {"frames_processed", capture.framesProcessed.load()},
        {"last_frame_rate_hz", capture.lastFrameRate.load()},
        {"last_data_rate_mbps", capture.lastDataRateMBps.load()},
        {"buffer_part_count", capture.bufferPartCount.load()},
        {"num_buffers", capture.numBuffers.load()},
    };

    j["processing"] = {
        {"running", processing.running.load()},
        {"realtime_running", processing.realtimeRunning.load()},
        {"experiment_active", processing.experimentActive.load()},
        {"worker_count", processing.workerCount.load()},
        {"jobs_queued", processing.jobsQueued.load()},
        {"jobs_processed", processing.jobsProcessed.load()},
        {"algo_fps", processing.algoFps.load()},
        {"valid_fps", processing.validFps.load()},
        {"invalid_fps", processing.invalidFps.load()},
        {"total_valid_flushed", processing.totalValidFlushed.load()},
        {"dropped_valid_frames", processing.droppedValidFrames.load()},
        {"target_group_objects", processing.targetGroupObjects.load()},
        {"unserved_target_group_objects", processing.unservedTargetGroupObjects.load()},
    };

    j["hdf5"] = {
        {"file_open", hdf5.fileOpen.load()},
        {"pending_valid", hdf5.pendingValid.load()},
        {"pending_invalid", hdf5.pendingInvalid.load()},
        {"appended_valid", hdf5.appendedValid.load()},
        {"appended_invalid", hdf5.appendedInvalid.load()},
        {"path", readSafeString(hdf5.pathMutex, hdf5.path, Hdf5Slot::kPathMax)},
    };

    j["frame_store"] = {
        {"capacity", frameStore.capacity.load()},
        {"total_written", frameStore.totalWritten.load()},
        {"total_filtered", frameStore.totalFiltered.load()},
        {"earliest_index", frameStore.earliestIndex.load()},
        {"latest_index", frameStore.latestIndex.load()},
    };

    j["autofocus"] = {
        {"connected", autofocus.connected.load()},
        {"enabled", autofocus.enabled.load()},
        {"voltage", autofocus.voltage.load()},
        {"ring_ratio_avg", autofocus.ringRatioAvg.load()},
        {"ring_ratio_median", autofocus.ringRatioMedian.load()},
        {"last_update_us", autofocus.lastUpdateUs.load()},
        {"com_port", readSafeString(autofocus.portMutex, autofocus.comPort, AutofocusSlot::kPortMax)},
    };

    j["syringe_pump"] = {
        {"sample", {
            {"connected", syringePump.sample.connected.load()},
            {"run_status", syringePump.sample.runStatus.load()},
            {"flow_rate", syringePump.sample.flowRate.load()},
            {"volume", syringePump.sample.volume.load()},
            {"stalled", syringePump.sample.stalled.load()},
        }},
        {"sheath", {
            {"connected", syringePump.sheath.connected.load()},
            {"run_status", syringePump.sheath.runStatus.load()},
            {"flow_rate", syringePump.sheath.flowRate.load()},
            {"volume", syringePump.sheath.volume.load()},
            {"stalled", syringePump.sheath.stalled.load()},
        }},
    };

    j["trigger"] = {
        {"running", trigger.running.load()},
        {"trigger_count", trigger.triggerCount.load()},
        {"last_onset_us", trigger.lastOnsetUs.load()},
        {"dropped_requests", trigger.droppedRequests.load()},
        {"dropped_pulses", trigger.droppedPulses.load()},
        {"target_latency_us", trigger.targetLatencyUs.load()},
    };

    j["recorder"] = {
        {"recording", recorder.recording.load()},
        {"frames_written", recorder.framesWritten.load()},
        {"frames_filtered", recorder.framesFiltered.load()},
    };

    return j.dump(2);
}
#else
// Minimal hand-rolled fallback if nlohmann_json is unavailable at compile time.
std::string CrashStateMirror::snapshotJsonString() const {
    std::ostringstream os;
    os << "{"
       << "\"timestamp_ms\":" << nowEpochMs()
       << ",\"capture\":{\"running\":" << capture.running.load()
       << ",\"frames_processed\":" << capture.framesProcessed.load()
       << ",\"last_frame_rate_hz\":" << capture.lastFrameRate.load()
       << ",\"last_data_rate_mbps\":" << capture.lastDataRateMBps.load()
       << "}"
       << ",\"processing\":{\"running\":" << processing.running.load()
       << ",\"realtime_running\":" << processing.realtimeRunning.load()
       << ",\"experiment_active\":" << processing.experimentActive.load()
       << ",\"jobs_queued\":" << processing.jobsQueued.load()
       << ",\"jobs_processed\":" << processing.jobsProcessed.load()
       << "}"
       << ",\"hdf5\":{\"file_open\":" << hdf5.fileOpen.load() << "}"
       << ",\"frame_store\":{\"total_written\":" << frameStore.totalWritten.load()
       << ",\"capacity\":" << frameStore.capacity.load() << "}"
       << "}";
    return os.str();
}
#endif

} // namespace backend::diagnostics
