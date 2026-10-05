// Start-of-run replay clip (issue #463, plan
// docs/exec-plans/active/2026-09-30-replay-clip-capture.md).
//
// Every experiment silently keeps the first min(maxFrames, maxDuration,
// maxBytes) frames after it enters Active, together with what is needed to
// reprocess them: the frozen run snapshot, the canonical processing config,
// the raw config.json and the background. A clip belongs to its recording:
// it is written next to the run's HDF5 file as <stem>.replay-clip/ (so it is
// deleted with the run and never expires on its own) and its frames/ folder
// is directly usable as a mock-camera folder. Its outcome is also recorded in
// the run's HDF5 provenance by the coordinator (provenanceJson()).
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
// - When the byte cap or the free-disk reserve would be exceeded the clip
//   stops there (frames already written are kept), is marked incomplete,
//   logged, and the reason reaches the run's provenance. Older clips are
//   never deleted to make room.
#pragma once

#include "backend/app/ExperimentReadiness.h"
#include "backend/diagnostics/MemoryBudget.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace cv {
class Mat;
}

namespace backend::playback {
class FrameStore;
}

namespace backend::recording {

// Defaults are the agreed policy (2026-10-05); every one is overridable via
// replayClipOptionsFromEnvironment() / setOptions().
struct ReplayClipOptions {
    bool enabled{true};                    // MIB_REPLAY_CLIP=0 disables
    uint64_t maxFrames{1000};              // MIB_REPLAY_CLIP_MAX_FRAMES; write indices spanned (copied + gaps)
    uint64_t maxDurationUs{1'000'000};     // MIB_REPLAY_CLIP_MAX_MS; host time since the first copied frame
    uint64_t maxBytes{512ULL << 20};       // MIB_REPLAY_CLIP_MAX_MB; clip cap (frame bytes held / written)
    uint64_t freeSpaceReserveBytes{512ULL << 20}; // MIB_REPLAY_CLIP_RESERVE_MB; must stay free on the run's volume
    uint64_t maxWriteBytesPerSec{64ULL << 20};    // MIB_REPLAY_CLIP_WRITE_MBPS; 0 = unthrottled
    std::chrono::milliseconds firstFrameTimeout{2000};
    std::chrono::milliseconds shutdownDrainTimeout{5000};
};

// `base` with every MIB_REPLAY_CLIP* environment override applied. A value
// that is not a non-negative integer is ignored with a warning.
ReplayClipOptions replayClipOptionsFromEnvironment(ReplayClipOptions base = {});

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
    // Fallback location for a run without an output path.
    const std::filesystem::path& rootDir() const { return rootDir_; }

    // Where the clip of the run writing `outputPath` goes:
    // <dir>/<stem>.replay-clip (a numeric suffix is added if it exists).
    static std::filesystem::path clipDirForRun(const std::string& outputPath);

    // Test seam: replaces the free-space query (bytes available on the
    // volume of the given path; nullopt = unknown, treated as enough).
    using FreeSpaceProbe = std::function<std::optional<uint64_t>(const std::filesystem::path&)>;
    void setFreeSpaceProbeForTests(FreeSpaceProbe probe);

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

    // The clip outcome of run `startGeneration` as JSON for the run's HDF5
    // provenance (/run_provenance @replay_clip_json). `final` is false while
    // the clip is still capturing or writing; the clip's manifest.json is
    // authoritative then.
    std::string provenanceJson(uint64_t startGeneration) const;

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
    // A run whose clip was skipped without touching status_ (busy).
    uint64_t lastSkipGeneration_{0};
    std::string lastSkipReason_;
    FreeSpaceProbe freeSpaceProbe_;

    diagnostics::ByteAccountant memory_;
};

} // namespace backend::recording
