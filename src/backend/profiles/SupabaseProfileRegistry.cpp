#include "backend/profiles/SupabaseProfileRegistry.h"
#include <nlohmann/json.hpp>

namespace backend::profiles {
namespace {
using Json = nlohmann::json;
Revision decode(const Json& j) {
    try {
        Revision r;
        r.methodId = j.at("method_id").get<std::string>();
        r.revisionId = j.at("revision_id").get<std::string>();
        r.projectId = j.at("project_id").get<std::string>();
        r.parentRevisionId = j.at("parent_revision_id").is_null()
                                 ? ""
                                 : j.at("parent_revision_id").get<std::string>();
        r.displayName = j.at("display_name").get<std::string>();
        r.authorId = j.at("author_id").get<std::string>();
        r.canonicalContent = j.at("canonical_content").get<std::string>();
        r.contentHash = j.at("content_hash").get<std::string>();
        for (const char* key : {"revision_number", "metadata_version"})
            if (!j.at(key).is_number_integer() || j.at(key).get<int64_t>() <= 0)
                throw RegistryError(RegistryErrorCode::Invalid, "Invalid server revision counter");
        r.revisionNumber = j.at("revision_number").get<uint64_t>();
        r.metadataVersion = j.at("metadata_version").get<uint64_t>();
        r.state = centralStateFromString(j.at("state").get<std::string>());
        verifyRevision(r);
        return r;
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Malformed registry revision");
    }
}
Json parse(const std::string& body) {
    try {
        return Json::parse(body);
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Malformed registry response");
    }
}
} // namespace
SupabaseProfileRegistry::SupabaseProfileRegistry(std::string origin, std::string key,
                                                 std::function<std::string()> token,
                                                 RegistryHttpTransport transport)
    : origin_(std::move(origin)), publishableKey_(std::move(key)),
      userAccessToken_(std::move(token)), transport_(std::move(transport)) {
    // Origin-only HTTPS endpoint; URL credentials, path/query and redirects are forbidden.
    if (origin_.compare(0, 8, "https://") != 0 || origin_.size() <= 8 ||
        origin_.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:", 8) !=
            std::string::npos ||
        publishableKey_.compare(0, 15, "sb_publishable_") != 0 ||
        publishableKey_.find_first_of("\r\n") != std::string::npos || !userAccessToken_ ||
        !transport_)
        throw RegistryError(RegistryErrorCode::Invalid,
                            "HTTPS registry origin, publishable key and user transport required");
}
std::string SupabaseProfileRegistry::rpc(const std::string& name, const std::string& body) {
    const auto token = userAccessToken_();
    if (token.empty() || token.find_first_of("\r\n") != std::string::npos)
        throw RegistryError(RegistryErrorCode::Authentication, "User authentication required");
    RegistryHttpRequest request{origin_ + "/rest/v1/rpc/" + name,
                                body,
                                {{"apikey", publishableKey_},
                                 {"Authorization", "Bearer " + token},
                                 {"Content-Type", "application/json"}}};
    const auto response = transport_(request);
    if (response.status == 0 || response.status == 408 || response.status == 429 ||
        response.status >= 500)
        throw RegistryError(RegistryErrorCode::Offline, "Registry temporarily unavailable");
    if (response.status == 401)
        throw RegistryError(RegistryErrorCode::Authentication, "Registry session expired");
    if (response.status == 403)
        throw RegistryError(RegistryErrorCode::Permission, "Registry access denied");
    if (response.status == 404)
        throw RegistryError(RegistryErrorCode::NotFound, "Registry revision not found");
    if (response.status == 409)
        throw RegistryError(RegistryErrorCode::Conflict,
                            "Registry revision conflict; refresh before retry");
    if (response.status < 200 || response.status >= 300)
        throw RegistryError(RegistryErrorCode::Invalid, "Registry request rejected");
    if (response.body.size() > request.maxResponseBytes)
        throw RegistryError(RegistryErrorCode::Invalid, "Registry response exceeds limit");
    return response.body;
}
RevisionPage SupabaseProfileRegistry::listRevisions(const std::string& project,
                                                    const std::string& cursor) {
    // One full revision per page bounds the response even at the content limit.
    auto j = parse(rpc("registry_list_revisions",
                       Json({{"p_project_id", project}, {"p_after", cursor}}).dump()));
    try {
        RevisionPage page;
        const auto& revisions = j.at("revisions");
        if (!revisions.is_array() || revisions.size() > 1)
            throw RegistryError(RegistryErrorCode::Invalid, "Invalid registry page");
        for (const auto& item : revisions)
            page.revisions.push_back(decode(item));
        page.nextCursor = j.at("next_cursor").get<std::string>();
        return page;
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Malformed registry page");
    }
}
Revision SupabaseProfileRegistry::fetchRevision(const std::string& id) {
    auto result =
        decode(parse(rpc("registry_fetch_revision", Json({{"p_revision_id", id}}).dump())));
    if (result.revisionId != id)
        throw RegistryError(RegistryErrorCode::Integrity, "Registry returned another revision");
    return result;
}
Revision SupabaseProfileRegistry::submit(const Revision& draft, const std::string& expectedHead) {
    // Server assigns author, revision number, state and timestamps from authenticated context.
    auto j = Json({{"p_method_id", draft.methodId},
                   {"p_revision_id", draft.revisionId},
                   {"p_parent_revision_id", draft.parentRevisionId},
                   {"p_expected_head", expectedHead},
                   {"p_content", draft.canonicalContent},
                   {"p_hash", draft.contentHash}});
    auto result = decode(parse(rpc("registry_submit", j.dump())));
    if (result.revisionId != draft.revisionId || result.methodId != draft.methodId ||
        result.contentHash != draft.contentHash ||
        result.canonicalContent != draft.canonicalContent)
        throw RegistryError(RegistryErrorCode::Integrity, "Submitted revision identity mismatch");
    return result;
}
Revision SupabaseProfileRegistry::transition(const std::string& id, CentralState state,
                                             uint64_t version, const std::string& reason) {
    auto result = decode(parse(rpc("registry_transition", Json({{"p_revision_id", id},
                                                                {"p_state", toString(state)},
                                                                {"p_expected_version", version},
                                                                {"p_reason", reason}})
                                                              .dump())));
    if (result.revisionId != id)
        throw RegistryError(RegistryErrorCode::Integrity, "Transition revision identity mismatch");
    return result;
}
} // namespace backend::profiles
