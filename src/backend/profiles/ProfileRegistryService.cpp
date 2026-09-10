#include "backend/profiles/ProfileRegistryService.h"
namespace backend::profiles {
bool ProfileRegistryService::attempt(const std::function<void()>& action) {
    try {
        action();
        health_.connectivity = RegistryHealth::Connectivity::Online;
        health_.message.clear();
        ++health_.successfulRequests;
        return true;
    } catch (const RegistryError& e) {
        ++health_.failedRequests;
        health_.message = e.what();
        switch (e.code) {
        case RegistryErrorCode::Offline:
            health_.connectivity = RegistryHealth::Connectivity::Offline;
            break;
        case RegistryErrorCode::Authentication:
            health_.connectivity = RegistryHealth::Connectivity::AuthenticationRequired;
            break;
        case RegistryErrorCode::Permission:
            health_.connectivity = RegistryHealth::Connectivity::PermissionDenied;
            break;
        default:
            health_.connectivity = RegistryHealth::Connectivity::Failed;
            break;
        }
        return false;
    }
}
bool ProfileRegistryService::download(const std::string& id) {
    return attempt([&] { cache_.store(registry_.fetchRevision(id)); });
}
std::optional<std::string> ProfileRegistryService::syncPage(const std::string& project,
                                                            const std::string& cursor) {
    std::string next;
    if (!attempt([&] {
            const auto page = registry_.listRevisions(project, cursor);
            if (!page.nextCursor.empty() && (page.nextCursor == cursor || page.revisions.empty()))
                throw RegistryError(RegistryErrorCode::Invalid,
                                    "Registry pagination did not advance");
            for (const auto& revision : page.revisions) {
                if (revision.projectId != project)
                    throw RegistryError(RegistryErrorCode::Integrity,
                                        "Registry returned another project's revision");
                cache_.store(revision);
            }
            next = page.nextCursor;
        }))
        return std::nullopt;
    return next;
}
} // namespace backend::profiles
