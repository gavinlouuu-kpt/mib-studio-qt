#pragma once
#include "backend/profiles/ProfileRegistry.h"
#include <functional>
#include <map>

namespace backend::profiles {
struct RegistryHttpRequest {
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
    unsigned timeoutMs{8000};
    size_t maxResponseBytes{2 * 1024 * 1024};
    // Optional: polled by the transport while a request is in flight; true
    // means abort now and return status 0 (lets shutdown/cancel skip the
    // remaining timeout).
    std::function<bool()> cancelled;
};
struct RegistryHttpResponse {
    unsigned status{0}; // zero indicates transport failure
    std::string body;
};
// Transport MUST enforce timeout, response cap, TLS verification and no
// redirects. Auth headers must never be logged. Injection permits deterministic
// offline/auth tests and platform-specific native HTTP without UI dependencies.
using RegistryHttpTransport = std::function<RegistryHttpResponse(const RegistryHttpRequest&)>;

class SupabaseProfileRegistry final : public ProfileRegistry {
public:
    SupabaseProfileRegistry(std::string origin, std::string publishableKey,
                            std::function<std::string()> userAccessToken,
                            RegistryHttpTransport transport);
    // Applied to every request from here on (worker cancellation seam).
    void setCancellation(std::function<bool()> cancelled) { cancelled_ = std::move(cancelled); }
    std::vector<RegistryProject> listProjects() override;
    RevisionPage listRevisions(const std::string& projectId,
                               const std::string& cursor = {}) override;
    Revision fetchRevision(const std::string& revisionId) override;
    Revision submit(const Revision& draft, const std::string& expectedHead) override;
    Revision transition(const std::string& revisionId, CentralState state,
                        uint64_t expectedMetadataVersion, const std::string& reason) override;
    std::vector<RegistryMethod> listMethods(const std::string& projectId) override;
    RegistryMethod createMethod(const std::string& projectId, const std::string& methodId,
                                const std::string& displayName,
                                const std::string& description) override;
    RevisionHistory revisionHistory(const std::string& revisionId) override;

private:
    std::string rpc(const std::string& name, const std::string& body);
    std::string origin_;
    std::string publishableKey_;
    std::function<std::string()> userAccessToken_;
    RegistryHttpTransport transport_;
    std::function<bool()> cancelled_;
};

// Shared by the RPC provider and SupabaseAuth: origin-only HTTPS URL, a
// publishable (never service-role) key, no header injection.
void validateSupabaseEndpoint(const std::string& origin, const std::string& publishableKey);
} // namespace backend::profiles
