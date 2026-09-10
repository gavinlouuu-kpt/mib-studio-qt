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
};
// Worker-confined service. A single explicit download or sync page is one
// bounded operation; no polling, thread ownership, selection, Apply or Start.
// Both shells must marshal commands to the same backend-owned worker in M1.
class ProfileRegistryService {
public:
    ProfileRegistryService(ProfileRegistry& registry, ProfileCache& cache)
        : registry_(registry), cache_(cache) {}
    bool download(const std::string& revisionId);
    // Pages contain immutable revision IDs. Restart a full scan on refresh;
    // never use the cursor as a metadata watermark (revocations update in place).
    std::optional<std::string> syncPage(const std::string& projectId,
                                        const std::string& cursor = {});
    const RegistryHealth& health() const { return health_; }

private:
    bool attempt(const std::function<void()>& action);
    ProfileRegistry& registry_;
    ProfileCache& cache_;
    RegistryHealth health_;
};
} // namespace backend::profiles
