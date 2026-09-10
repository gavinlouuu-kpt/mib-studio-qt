#pragma once

#include "backend/profiles/ProfileRegistry.h"
#include <memory>

namespace backend::profiles {
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
    void recordValidation(const LocalValidation& validation);
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
