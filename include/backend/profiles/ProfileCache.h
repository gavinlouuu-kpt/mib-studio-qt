#pragma once

#include "backend/profiles/ProfileRegistry.h"
#include <memory>

namespace backend::profiles {
// A stored local validation plus when it was recorded (SQLite UTC
// "YYYY-MM-DD HH:MM:SS").
struct LocalValidationRecord {
    LocalValidation validation;
    std::string validatedAtUtc;
};

// A local, never-auto-published draft of a method revision (#398 M3). The
// method and revision IDs are generated when the draft is created so a
// submission retried after a dropped response is idempotent on the server.
struct MethodDraft {
    std::string draftId;
    std::string projectId;
    std::string methodId;
    bool newMethod{false};          // create the method on submit
    std::string methodDisplayName;
    std::string methodDescription;  // new methods only
    std::string baseRevisionId;     // parent; empty for a new method
    std::string configJson;
    std::string cameraScript;
    std::string processingCoreId;
    int processingContractVersion{1};
    std::string hardwareCompatibilityJson{"{}"};
    std::string releaseNotes;
    std::string revisionId;         // pre-generated for the submission
    std::string submittedRevisionId; // set once submitted; the draft is then read-only
    std::string createdAtUtc;
    std::string updatedAtUtc;
};

// One instance/SQLite connection per registry worker. Database is scoped to
// one registry origin + authenticated subject. No access tokens are persisted.
// No background threads, eviction, selected profile, or hardware side effects.
class ProfileCache {
public:
    ProfileCache(const std::string& path, const std::string& registryOrigin,
                 const std::string& subjectId);
    ~ProfileCache();
    ProfileCache(const ProfileCache&) = delete;
    ProfileCache& operator=(const ProfileCache&) = delete;

    void store(const Revision& revision);
    Revision read(const std::string& revisionId) const;
    std::vector<Revision> list(const std::string& projectId) const;
    // Every cached revision. Rows failing verification are skipped and their
    // IDs reported in `corrupt` (never silently dropped, never returned).
    std::vector<Revision> listAll(std::vector<std::string>* corrupt = nullptr) const;
    void recordValidation(const LocalValidation& validation);
    // Every recorded validation, newest first.
    std::vector<LocalValidationRecord> listValidations() const;
    // Local drafts (#398 M3). saveDraft inserts or replaces by draftId and
    // refuses to change a submitted draft; newest first.
    void saveDraft(const MethodDraft& draft);
    MethodDraft readDraft(const std::string& draftId) const;
    std::vector<MethodDraft> listDrafts() const;
    void deleteDraft(const std::string& draftId);
    void markDraftSubmitted(const std::string& draftId, const std::string& revisionId);
    Eligibility eligibility(const std::string& revisionId, const std::string& instrumentId,
                            const std::string& contextHash) const;
    // Revocation is sticky. Historical reads remain available. A server must
    // create a new revision to restore execution eligibility after revocation.
    void updateState(const std::string& revisionId, CentralState state, uint64_t metadataVersion);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace backend::profiles
