// Start-of-run replay clip (issue #463, plan
// docs/exec-plans/active/2026-09-30-replay-clip-capture.md).
//
// Every experiment silently keeps the first min(maxFrames, maxDuration,
// maxBytes) frames after it enters Active, together with what is needed to
// reprocess them: the frozen run snapshot, the canonical processing config,
// the raw config.json and the background. The clip is written under
// <dataDir>/replay-clips/<utc>-g<startGeneration>/ and its frames/ folder is
// directly usable as a mock-camera folder.
//
// Invariants:
// - arm() is O(1) apart from starting one worker thread; it never blocks or
//   fails the experiment Start. Every clip error is recorded in the clip's
//   manifest and the log, never reported to the experiment.
// - Frames are copied by write index from the shared FrameStore on the
//   worker thread (no capture callback, no whole-ring lock). A frame that
//   could not be copied becomes an explicit gap record; the clip is then
//   marked non-contiguous, never silently shortened.
// - The experiment HDF5 file is never touched (#451 writer ownership).
// - Memory held by the clip buffer is bounded by maxBytes and reported
//   through memoryStats() (#370).
#pragma once

#include "backend/app/ExperimentReadiness.h"
#include "backend/diagnostics/MemoryBudget.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cv {
class Mat;
}

namespace backend::playback {
class FrameStore;
}

namespace backend::recording {

struct ReplayClipOptions {
    bool enabled{true};
    uint64_t maxFrames{1000};              // write indices spanned (copied + gaps)
    uint64_t maxDurationUs{1'000'000};     // host time since the first copied frame
    uint64_t maxBytes{512ULL << 20};       // frame bytes held in memory / written
    uint64_t freeSpaceReserveBytes{512ULL << 20}; // must stay free beyond the clip
    uint64_t maxWriteBytesPerSec{64ULL << 20};    // 0 = unthrottled
    std::chrono::milliseconds firstFrameTimeout{2000};
    std::chrono::milliseconds shutdownDrainTimeout{5000};
};

enum class ReplayClipState {
    Idle,      // no clip has been armed
    Capturing, // copying frames from the FrameStore
    Writing,   // encoding/writing the copied frames
    Complete,  // every frame in the window was written
    Incomplete, // written, but ended early (run ended / shutdown) or has gaps
    Skipped,   // not captured (disabled, busy, no space, no frames)
    Failed,    // could not be written
};

const char* toString(ReplayClipState s);

// Everything the coordinator hands over at the moment the run enters Active.
struct ReplayClipArm {
    std::shared_ptr<playback::FrameStore> store;
    uint64_t firstWriteIndex{0}; // FrameStore::committedCount() at Active
    app::RunConfigurationSnapshot run;
    std::string runSnapshotJson;           // runSnapshotToJson(run)
    std::string canonicalProcessingConfig; // hashes to run.processingConfigSha256
    std::string configJson;                // hashes to run.configJsonSha256
    std::shared_ptr<const cv::Mat> background;
};

struct ReplayClipStatus {
    ReplayClipState state{ReplayClipState::Idle};
    uint64_t startGeneration{0};
    std::string clipDir;      // empty when nothing was written
    std::string endReason;    // frame_limit | duration_limit | byte_limit | run_ended | shutdown | ...
    std::string message;      // human-readable detail for skipped/failed
    uint64_t firstWriteIndex{0};
    uint64_t framesCopied{0};
    uint64_t gaps{0};
    uint64_t framesWritten{0};
    uint64_t writeFailures{0};
    uint64_t bytesCopied{0};
    bool contiguous{true};
    bool configVerified{false};
};

class ReplayClipRecorder {
public:
    explicit ReplayClipRecorder(std::filesystem::path rootDir, ReplayClipOptions options = {});
    ~ReplayClipRecorder();

    ReplayClipRecorder(const ReplayClipRecorder&) = delete;
    ReplayClipRecorder& operator=(const ReplayClipRecorder&) = delete;

    void setOptions(const ReplayClipOptions& options);
    ReplayClipOptions options() const;
    const std::filesystem::path& rootDir() const { return rootDir_; }

    // Start a clip for the run that just entered Active. Returns false (and
    // records a Skipped status) when disabled, shut down, or the previous
    // clip is still being written. Never blocks on I/O.
    bool arm(ReplayClipArm arm);

    // The run left Active: close the capture window now if it is still open.
    // Frames already copied are kept; the clip is marked incomplete.
    void notifyRunEnded(uint64_t startGeneration);

    // Wait until no clip is capturing or writing. For tests and tools.
    bool waitIdle(std::chrono::milliseconds timeout);

    // Bounded and idempotent: closes the capture window, lets the writer
    // drain for shutdownDrainTimeout, then abandons remaining frames (the
    // manifest says so) and joins the worker.
    void shutdown();

    ReplayClipStatus lastStatus() const;
    diagnostics::MemoryOwnerStats memoryStats() const;

private:
    void run(ReplayClipArm arm);
    void setStatus(const ReplayClipStatus& status);

    const std::filesystem::path rootDir_;

    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    ReplayClipOptions options_;
    ReplayClipStatus status_;
    std::thread worker_;
    bool busy_{false};
    bool shutdown_{false};
    uint64_t activeGeneration_{0};
    bool closeWindow_{false}; // run ended / shutdown: stop copying
    std::string closeReason_;
    bool abortWrite_{false};  // shutdown drain expired: stop writing

    diagnostics::ByteAccountant memory_;
};

} // namespace backend::recording
