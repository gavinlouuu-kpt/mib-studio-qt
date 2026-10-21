// The instrument's calibration records (#716 A4a, bridge ABI 42): a small, append-only,
// versioned store in the instrument settings (<dataDir>/instrument_calibration.json, beside
// instrument_identity.json; the PZ7035 eMMC rule allows settings). A status change is a new
// version of the record, never an edit; nothing is ever removed. Qt-free.
//
// Writes are atomic (a temp file, flushed, renamed over the store): a failed write changes
// nothing, on disk or in memory. A store file that cannot be read is left untouched and the store
// refuses every append (error() says why), so a corrupt file never loses its history silently.
// Bounded at kCalibrationStoreMaxVersions versions: past it appends are refused with the reason.
//
// Thread-safe (one mutex; it never calls out). Lock order: the experiment coordinator, then the
// store.
#pragma once

#include "backend/app/CalibrationRecord.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace backend::app {

// Contract-pinned (bridge-contract.json calibration_apply_outcomes); append only.
enum class CalibrationApplyOutcome {
    Applied = 0,
    NotFound = 1,          // no record has that id
    Insufficient = 2,      // an insufficient record can never be applied
    NotApplicable = 3,     // superseded, invalidated, or already applied and in effect
    StaleFactor = 4,       // expected_current_factor is not the current factor
    RunActive = 5,         // an experiment is not idle (or raw recording runs)
    IdentityMismatch = 6,  // derived under another camera / ROI / chip / objective, or the current identity is unknown
    StoreFailed = 7,       // the store is unreadable, full, or the write failed (nothing changed)
    InvalidRequest = 8,    // a malformed record id or expected factor
};
const char* toString(CalibrationApplyOutcome outcome);

struct CalibrationApplyResult {
    CalibrationApplyOutcome outcome{CalibrationApplyOutcome::InvalidRequest};
    std::string message;
    std::string recordId;
    std::optional<double> previousFactor;   // the factor before (Applied)
    std::optional<double> newFactor;        // the factor after (Applied)
    std::string supersededRecordId;         // the previously applied record, now superseded
    bool ok() const { return outcome == CalibrationApplyOutcome::Applied; }
};

class CalibrationStore {
public:
    // Loads `file` when it exists (an absent file is an empty store).
    explicit CalibrationStore(std::filesystem::path file);

    const std::filesystem::path& path() const { return file_; }
    // Empty when the store is usable; otherwise why it refuses appends.
    std::string error() const;
    // Bumps on every append (the number of versions held).
    uint64_t revision() const;

    // Newest version of every record, newest record first.
    std::vector<CalibrationRecord> latestRecords() const;
    std::optional<CalibrationRecord> latest(const std::string& recordId) const;
    // Every version of one record, oldest first.
    std::vector<CalibrationRecord> history(const std::string& recordId) const;
    // The record whose newest version is Applied (at most one).
    std::optional<CalibrationRecord> applied() const;

    // Append a newly derived record (status Proposed or Insufficient, version 1).
    bool addDerived(CalibrationRecord record, std::string* why = nullptr);

    // Hard invalidation: when the applied record's identity is known and differs from `current`
    // (a known identity), append its Invalidated version. Returns true when it did; `note`
    // receives what changed. An unknown current identity never invalidates.
    bool invalidateOnIdentityChange(const CalibrationIdentity& current, uint64_t nowUnixMs,
                                    std::string* note = nullptr);

    // The store half of calibration_apply: checks the record (exists, not insufficient, proposed,
    // or applied but not in effect; identity equal to `current`, which must be known), the
    // compare-and-set (`expectedCurrentFactor` against `currentFactor`), then appends the previous
    // applied record's Superseded version and this record's Applied version in one write. The
    // caller holds the coordinator's idle transaction, read `currentFactor` inside it, and sets
    // the factor only when this returns Applied.
    CalibrationApplyResult apply(const std::string& recordId, double expectedCurrentFactor, double currentFactor,
                                 const CalibrationIdentity& current, uint64_t nowUnixMs);

    // Tests: make the next writes fail (fault injection).
    void setFailWritesForTesting(bool fail);

private:
    bool appendLocked(std::vector<CalibrationRecord> versions, std::string* why);
    std::optional<CalibrationRecord> latestLocked(const std::string& recordId) const;
    std::optional<CalibrationRecord> appliedLocked() const;
    void load();

    const std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<CalibrationRecord> versions_; // append order
    std::string error_;
    bool failWrites_{false};
};

} // namespace backend::app
