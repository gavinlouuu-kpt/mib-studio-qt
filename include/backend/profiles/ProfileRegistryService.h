#pragma once
#include "backend/profiles/ProfileCache.h"
#include <functional>

namespace backend::profiles {
struct RegistryHealth {
    enum class Connectivity {
        Unknown,
        Online,
        Offline,
        AuthenticationRequired,
        PermissionDenied,
        Failed
    };
    Connectivity connectivity{Connectivity::Unknown};
    std::string message;
    uint64_t successfulRequests{0};
    uint64_t failedRequests{0};
    // Listed revisions that failed verification and were skipped, not cached.
    uint64_t rejectedRevisions{0};
    std::string lastRejection;
};
// Worker-confined service. A single explicit download or sync page is one
// bounded operation; no polling, thread ownership, selection, Apply or Start.
// Both shells must marshal commands to the same backend-owned worker in M1.
class ProfileRegistryService {
public:
    ProfileRegistryService(ProfileRegistry& registry, ProfileCache& cache)
        : registry_(registry), cache_(cache) {}
    bool download(const std::string& revisionId);
    std::optional<std::vector<RegistryProject>> listProjects();
    // Pages contain immutable revision IDs. Restart a full scan on refresh;
    // never use the cursor as a metadata watermark (revocations update in place).
    std::optional<std::string> syncPage(const std::string& projectId,
                                        const std::string& cursor = {});
    // #398 M3 authoring; each is one bounded request (nullopt / false on
    // failure, health() says why). Results are cached where they are revisions.
    std::optional<std::vector<RegistryMethod>> listMethods(const std::string& projectId);
    std::optional<RegistryMethod> createMethod(const std::string& projectId,
                                               const std::string& methodId,
                                               const std::string& displayName,
                                               const std::string& description);
    std::optional<Revision> submit(const Revision& draft, const std::string& expectedHead);
    std::optional<Revision> transition(const std::string& revisionId, CentralState state,
                                       uint64_t expectedMetadataVersion, const std::string& reason);
    std::optional<RevisionHistory> history(const std::string& revisionId);
    // Code of the last failure (valid after a failed call).
    RegistryErrorCode lastErrorCode() const { return lastErrorCode_; }
    const RegistryHealth& health() const { return health_; }

private:
    bool attempt(const std::function<void()>& action);
    ProfileRegistry& registry_;
    ProfileCache& cache_;
    RegistryHealth health_;
    RegistryErrorCode lastErrorCode_{RegistryErrorCode::Invalid};
};
} // namespace backend::profiles
