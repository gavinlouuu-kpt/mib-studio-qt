#include "backend/app/PersistenceOwnership.h"

#include "backend/services/CrashReporter.h"

#include <spdlog/spdlog.h>

namespace backend::app {

namespace {
// Path-free breadcrumb payload: requested operation, owner/state, result, run id.
std::string breadcrumbJson(const char* result, PersistenceOwner requester, const std::string& operation,
                           PersistenceOwner owner, uint64_t runId)
{
    return std::string("{\"result\":\"") + result + "\",\"requester\":\"" + toString(requester) +
           "\",\"operation\":\"" + operation + "\",\"owner\":\"" + toString(owner) +
           "\",\"runId\":" + std::to_string(runId) + "}";
}
} // namespace

const char* toString(PersistenceOwner owner)
{
    switch (owner) {
    case PersistenceOwner::None: return "none";
    case PersistenceOwner::Experiment: return "experiment";
    case PersistenceOwner::FrameRecording: return "frameRecording";
    case PersistenceOwner::ReviewLoad: return "reviewLoad";
    }
    return "unknown";
}

std::string PersistenceOwnership::conflictMessage(PersistenceOwner requester, PersistenceOwner holder)
{
    if (requester == PersistenceOwner::FrameRecording && holder == PersistenceOwner::Experiment)
        return "An experiment is running and owns the HDF5 file. Stop the experiment and wait for it "
               "to finish saving before starting manual recording.";
    if (requester == PersistenceOwner::FrameRecording && holder == PersistenceOwner::FrameRecording)
        return "Frame recording is already in progress (or still saving). Stop it before starting "
               "a new recording.";
    if (requester == PersistenceOwner::Experiment && holder == PersistenceOwner::FrameRecording)
        return "Manual frame recording is active (or still saving). Stop recording before starting "
               "an experiment.";
    if (requester == PersistenceOwner::Experiment && holder == PersistenceOwner::Experiment)
        return "An experiment is already running or still finalizing.";
    if (requester == PersistenceOwner::ReviewLoad)
        return std::string("Cannot load a file for review while ") +
               (holder == PersistenceOwner::Experiment ? "an experiment" : "a recording") +
               " is writing. Stop it first.";
    return std::string("The HDF5 writer is busy (") + toString(holder) + ").";
}

PersistenceClaim PersistenceOwnership::tryAcquire(PersistenceOwner who, const std::string& operation)
{
    PersistenceClaim claim;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (owner_ == PersistenceOwner::None) {
            owner_ = who;
            runId_ = ++nextRunId_;
            operation_ = operation;
            claim.granted = true;
            claim.runId = runId_;
        } else {
            claim.holder = owner_;
            claim.holderRunId = runId_;
            claim.holderOperation = operation_;
            claim.message = conflictMessage(who, owner_);
        }
    }
    if (claim.granted) {
        SPDLOG_INFO("Persistence: {} granted to {} (run {})", operation, toString(who), claim.runId);
        services::CrashReporter::breadcrumb(
            "persistence", "writer claim granted",
            breadcrumbJson("granted", who, operation, who, claim.runId));
    } else {
        SPDLOG_WARN("Persistence: {} by {} rejected; writer owned by {} (run {}, {})", operation,
                    toString(who), toString(claim.holder), claim.holderRunId, claim.holderOperation);
        services::CrashReporter::breadcrumb(
            "persistence", "writer claim rejected",
            breadcrumbJson("rejected", who, operation, claim.holder, claim.holderRunId));
    }
    return claim;
}

bool PersistenceOwnership::release(PersistenceOwner who, uint64_t runId)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (owner_ != who || runId_ != runId || who == PersistenceOwner::None) {
            SPDLOG_WARN("Persistence: ignored release by {} (run {}); owner is {} (run {})", toString(who),
                        runId, toString(owner_), runId_);
            return false;
        }
        owner_ = PersistenceOwner::None;
        operation_.clear();
    }
    SPDLOG_INFO("Persistence: {} released the writer (run {})", toString(who), runId);
    services::CrashReporter::breadcrumb("persistence", "writer claim released",
                                        breadcrumbJson("released", who, "release", who, runId));
    return true;
}

PersistenceOwnership::Snapshot PersistenceOwnership::snapshot() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return Snapshot{owner_, runId_, operation_};
}

} // namespace backend::app
