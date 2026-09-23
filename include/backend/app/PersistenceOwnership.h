// Single-owner admission for the shared HDF5 writer (issue #451).
//
// AppBackend has exactly one Hdf5Service. An experiment run
// (ExperimentCoordinator) and manual raw-frame recording
// (AppBackend::startFrameRecording) both write through it, so they are
// mutually exclusive: whoever claims the writer first owns it until its file
// is closed, and every other start is rejected BEFORE it touches files,
// processing configuration, accounting or capture. Concurrent modes would
// need isolated writers and resource budgets and are out of scope.
//
// The claim is taken and checked under one mutex (no check-then-act). Each
// grant carries a monotonic run id; release() only frees the claim whose
// owner AND id match, so a stale or duplicate release can never free a
// newer owner's claim. PersistenceLease is the RAII form for paths that hand
// ownership to a worker thread.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace backend::app {

enum class PersistenceOwner {
    None = 0,
    Experiment = 1,      // ExperimentCoordinator run: Starting -> Active -> Stopping/finalized
    FrameRecording = 2,  // manual raw-frame recording: start -> writer thread closes its file
    ReviewLoad = 3,      // transient: bridge RecordingLoad replacing the reviewed file
};

const char* toString(PersistenceOwner owner);

struct PersistenceClaim {
    bool granted{false};
    uint64_t runId{0};                            // the grant's id when granted
    PersistenceOwner holder{PersistenceOwner::None}; // current owner when rejected
    uint64_t holderRunId{0};
    std::string holderOperation;
    std::string message; // actionable, path-free rejection text
};

class PersistenceOwnership {
public:
    struct Snapshot {
        PersistenceOwner owner{PersistenceOwner::None};
        uint64_t runId{0};
        std::string operation;
    };

    // Atomic admission. `operation` is a short, path-free label recorded for
    // diagnostics ("experiment start", "frame recording start", ...).
    PersistenceClaim tryAcquire(PersistenceOwner who, const std::string& operation);
    // Frees the claim only when (who, runId) is the current owner. Returns
    // false (and changes nothing) otherwise.
    bool release(PersistenceOwner who, uint64_t runId);

    Snapshot snapshot() const;
    bool isHeld() const { return snapshot().owner != PersistenceOwner::None; }

    // Rejection text for `requester` while `holder` owns the writer.
    static std::string conflictMessage(PersistenceOwner requester, PersistenceOwner holder);

private:
    mutable std::mutex mutex_;
    PersistenceOwner owner_{PersistenceOwner::None};
    uint64_t runId_{0};
    uint64_t nextRunId_{0};
    std::string operation_;
};

// Move-only RAII claim: releases on destruction unless release()d earlier.
class PersistenceLease {
public:
    PersistenceLease() = default;
    PersistenceLease(PersistenceOwnership& ownership, PersistenceOwner who, uint64_t runId)
        : ownership_(&ownership), who_(who), runId_(runId) {}
    ~PersistenceLease() { release(); }
    PersistenceLease(PersistenceLease&& o) noexcept { *this = std::move(o); }
    PersistenceLease& operator=(PersistenceLease&& o) noexcept
    {
        if (this != &o) {
            release();
            ownership_ = o.ownership_;
            who_ = o.who_;
            runId_ = o.runId_;
            o.ownership_ = nullptr;
        }
        return *this;
    }
    PersistenceLease(const PersistenceLease&) = delete;
    PersistenceLease& operator=(const PersistenceLease&) = delete;

    bool held() const { return ownership_ != nullptr; }
    uint64_t runId() const { return runId_; }
    void release()
    {
        if (ownership_) {
            ownership_->release(who_, runId_);
            ownership_ = nullptr;
        }
    }

private:
    PersistenceOwnership* ownership_{nullptr};
    PersistenceOwner who_{PersistenceOwner::None};
    uint64_t runId_{0};
};

} // namespace backend::app
