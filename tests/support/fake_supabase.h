// Minimal in-process Supabase (Auth + registry RPC) behind a RegistryHttpTransport,
// for registry worker and UI tests (#398). Users, single-use rotating refresh
// tokens, memberships and one-revision pages; knobs for an outage, an early
// 401, and a request that hangs until the caller cancels it. Thread-safe.
#pragma once

#include "backend/profiles/ProfileRegistry.h"
#include "backend/profiles/SupabaseProfileRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mib::test {

using namespace backend::profiles;
using Json = nlohmann::json;
using namespace std::chrono_literals;

inline constexpr const char* kOrigin = "https://registry.example";
inline constexpr const char* kKey = "sb_publishable_test";

inline Json revisionJson(const std::string& project, const std::string& id, uint64_t number,
                         const std::string& state = "published") {
    const auto content =
        canonicalMethod(R"({"config_schema_version":1,"gain":)" + std::to_string(number) + "}",
                        "camera();", "core", 1);
    return {{"method_id", "method-" + project},
            {"revision_id", id},
            {"project_id", project},
            {"parent_revision_id", nullptr},
            {"display_name", "Method " + project},
            {"author_id", "alice"},
            {"canonical_content", content},
            {"content_hash", contentHash(content)},
            {"revision_number", number},
            {"metadata_version", 1},
            {"state", state}};
}

// Minimal Supabase stand-in. Thread-safe: the worker thread calls handle(),
// the test thread flips knobs.
class FakeSupabase {
public:
    struct User {
        std::string id;
        std::string password;
        std::vector<std::string> projects;
        // Same roles in every member project (#398 M3 authoring checks).
        std::vector<std::string> roles{"operator"};
    };
    struct Method {
        std::string methodId;
        std::string displayName;
        std::string description;
        std::string head; // "" until a revision is published
    };

    std::mutex mutex;
    std::map<std::string, User> users;                  // email -> user
    std::map<std::string, std::string> accessTokens;    // token -> user id
    std::map<std::string, std::string> refreshTokens;   // token -> user id (single use)
    std::map<std::string, std::vector<Json>> revisions; // project -> sorted by id
    // #398 M3: methods created through the RPC (methods implied by fixture
    // revisions are derived), and per-revision reviews / audit events.
    std::map<std::string, std::vector<Method>> methods; // project -> methods
    std::map<std::string, Json> reviews;                // revision -> array
    std::map<std::string, Json> events;                 // revision -> array
    std::atomic<int> submits{0};
    int tokenCounter = 0;
    int expiresIn = 3600;
    bool offline = false;
    bool rejectNextAccess = false; // next RPC answers 401 (token revoked early)
    std::atomic<bool> hang{false}; // block until the request is cancelled
    std::atomic<int> hungRequests{0};
    std::atomic<int> requests{0};
    std::vector<std::string> paths;

    RegistryHttpResponse handle(const RegistryHttpRequest& request) {
        ++requests;
        if (hang) {
            ++hungRequests;
            while (!(request.cancelled && request.cancelled()))
                std::this_thread::sleep_for(5ms);
            return {0, {}};
        }
        std::lock_guard<std::mutex> lock(mutex);
        const auto path = request.url.substr(std::string(kOrigin).size());
        paths.push_back(path);
        if (request.headers.at("apikey") != kKey) return {401, "{}"};
        if (offline) return {0, {}};
        const auto body = request.body.empty() ? Json::object() : Json::parse(request.body);
        if (path == "/auth/v1/token?grant_type=password") {
            const auto it = users.find(body.at("email").get<std::string>());
            if (it == users.end() || it->second.password != body.at("password"))
                return {400, R"({"error_code":"invalid_credentials"})"};
            return session(it->first, it->second.id);
        }
        if (path == "/auth/v1/token?grant_type=refresh_token") {
            const auto it = refreshTokens.find(body.at("refresh_token").get<std::string>());
            if (it == refreshTokens.end())
                return {400, R"({"error_code":"refresh_token_not_found"})"};
            const auto userId = it->second;
            refreshTokens.erase(it); // rotation: a refresh token is single use
            for (const auto& [email, user] : users)
                if (user.id == userId) return session(email, userId);
            return {400, "{}"};
        }
        const auto auth = request.headers.find("Authorization");
        if (auth == request.headers.end() || auth->second.rfind("Bearer ", 0) != 0)
            return {401, "{}"};
        const auto token = auth->second.substr(7);
        const auto holder = accessTokens.find(token);
        if (holder == accessTokens.end()) return {401, "{}"};
        if (path == "/auth/v1/logout") {
            accessTokens.erase(holder);
            return {204, {}};
        }
        if (rejectNextAccess) {
            rejectNextAccess = false;
            accessTokens.erase(holder);
            return {401, "{}"};
        }
        const auto& member = userById(holder->second);
        if (path == "/rest/v1/rpc/registry_list_projects") {
            Json projects = Json::array();
            for (const auto& p : member.projects)
                projects.push_back(
                    {{"project_id", p}, {"display_name", "Project " + p}, {"roles", member.roles}});
            return {200, Json({{"projects", projects}}).dump()};
        }
        if (path == "/rest/v1/rpc/registry_list_revisions") {
            const auto project = body.at("p_project_id").get<std::string>();
            const auto after = body.at("p_after").get<std::string>();
            if (std::find(member.projects.begin(), member.projects.end(), project) ==
                member.projects.end())
                return {200, R"({"revisions":[],"next_cursor":""})"};
            for (const auto& r : revisions[project])
                if (after.empty() || r.at("revision_id").get<std::string>() > after)
                    return {200, Json({{"revisions", Json::array({r})},
                                       {"next_cursor", r.at("revision_id")}})
                                     .dump()};
            return {200, R"({"revisions":[],"next_cursor":""})"};
        }
        if (path == "/rest/v1/rpc/registry_list_methods") {
            const auto project = body.at("p_project_id").get<std::string>();
            Json out = Json::array();
            if (isMember(member, project))
                for (const auto& m : methodsOf(project))
                    out.push_back({{"method_id", m.methodId},
                                   {"project_id", project},
                                   {"display_name", m.displayName},
                                   {"description", m.description},
                                   {"head_revision_id", m.head.empty() ? Json(nullptr) : Json(m.head)}});
            return {200, Json({{"methods", out}}).dump()};
        }
        if (path == "/rest/v1/rpc/registry_create_method") {
            const auto project = body.at("p_project_id").get<std::string>();
            const auto id = body.at("p_method_id").get<std::string>();
            if (!isMember(member, project) || !hasRole(member, "author")) return {403, "{}"};
            for (const auto& m : methods[project])
                if (m.methodId == id)
                    return {200, Json({{"method_id", id}, {"project_id", project},
                                       {"display_name", m.displayName}, {"description", m.description},
                                       {"head_revision_id", m.head.empty() ? Json(nullptr) : Json(m.head)}})
                                     .dump()};
            methods[project].push_back({id, body.at("p_display_name").get<std::string>(),
                                        body.value("p_description", std::string{}), ""});
            return {200, Json({{"method_id", id}, {"project_id", project},
                               {"display_name", body.at("p_display_name")},
                               {"description", body.value("p_description", std::string{})},
                               {"head_revision_id", nullptr}})
                             .dump()};
        }
        if (path == "/rest/v1/rpc/registry_submit") {
            ++submits;
            const auto methodId = body.at("p_method_id").get<std::string>();
            std::string project;
            for (const auto& p : member.projects)
                for (const auto& m : methodsOf(p))
                    if (m.methodId == methodId) project = p;
            if (project.empty() || !hasRole(member, "author")) return {403, "{}"};
            const auto id = body.at("p_revision_id").get<std::string>();
            if (const auto* existing = find(id)) {
                if (existing->at("canonical_content") == body.at("p_content") &&
                    existing->value("release_notes", std::string{}) == body.value("p_release_notes", std::string{}))
                    return {200, existing->dump()};
                return {409, "{}"};
            }
            if (headOf(project, methodId) != body.at("p_expected_head").get<std::string>()) return {409, "{}"};
            uint64_t number = 0;
            for (const auto& r : revisions[project])
                if (r.at("method_id") == methodId) number = std::max(number, r.at("revision_number").get<uint64_t>());
            const auto parent = body.at("p_parent_revision_id").get<std::string>();
            Json r = {{"method_id", methodId},
                      {"revision_id", id},
                      {"project_id", project},
                      {"parent_revision_id", parent.empty() ? Json(nullptr) : Json(parent)},
                      {"display_name", displayNameOf(project, methodId)},
                      {"author_id", holder->second},
                      {"canonical_content", body.at("p_content")},
                      {"content_hash", body.at("p_hash")},
                      {"revision_number", number + 1},
                      {"metadata_version", 1},
                      {"state", "submitted"},
                      {"release_notes", body.value("p_release_notes", std::string{})}};
            insertSorted(project, r);
            event(id, holder->second, "submitted", "");
            return {200, r.dump()};
        }
        if (path == "/rest/v1/rpc/registry_transition") {
            const auto id = body.at("p_revision_id").get<std::string>();
            Json* r = find(id);
            if (!r || !isMember(member, r->at("project_id").get<std::string>())) return {403, "{}"};
            const auto target = body.at("p_state").get<std::string>();
            const bool review = target == "approved" || target == "rejected";
            if (!hasRole(member, review ? "reviewer" : "publisher")) return {403, "{}"};
            if (body.at("p_expected_version").get<uint64_t>() != r->at("metadata_version").get<uint64_t>())
                return {409, "{}"};
            const auto state = r->at("state").get<std::string>();
            const bool valid = (state == "submitted" && review) || (state == "approved" && target == "published") ||
                               ((state == "published" || state == "superseded") &&
                                (target == "archived" || target == "revoked")) ||
                               (state == "archived" && target == "revoked");
            if (!valid) return {409, "{}"};
            const auto project = r->at("project_id").get<std::string>();
            const auto methodId = r->at("method_id").get<std::string>();
            if (review) {
                if (r->at("author_id") == holder->second) return {403, "{}"};
                reviews[id].push_back({{"reviewer_id", holder->second}, {"decision", target},
                                       {"reason", body.at("p_reason")}, {"content_hash", r->at("content_hash")},
                                       {"created_at", "2026-10-04T10:00:00Z"}});
            }
            if (target == "published") {
                const auto parent = r->at("parent_revision_id").is_null() ? std::string{}
                                                                          : r->at("parent_revision_id").get<std::string>();
                const auto head = headOf(project, methodId);
                if (parent != head) return {409, "{}"};
                if (Json* old = head.empty() ? nullptr : find(head); old && old->at("state") == "published") {
                    (*old)["state"] = "superseded";
                    (*old)["metadata_version"] = old->at("metadata_version").get<uint64_t>() + 1;
                    event(head, holder->second, "superseded", body.at("p_reason").get<std::string>());
                }
                setHead(project, methodId, id);
            }
            r = find(id); // insertions never happen here, but stay safe
            (*r)["state"] = target;
            (*r)["metadata_version"] = r->at("metadata_version").get<uint64_t>() + 1;
            event(id, holder->second, target, body.at("p_reason").get<std::string>());
            return {200, r->dump()};
        }
        if (path == "/rest/v1/rpc/registry_revision_history") {
            const auto id = body.at("p_revision_id").get<std::string>();
            const Json* r = find(id);
            if (!r || !isMember(member, r->at("project_id").get<std::string>())) return {404, "{}"};
            return {200, Json({{"reviews", reviews.count(id) ? reviews[id] : Json::array()},
                               {"events", events.count(id) ? events[id] : Json::array()}})
                             .dump()};
        }
        if (path == "/rest/v1/rpc/registry_fetch_revision") {
            const auto id = body.at("p_revision_id").get<std::string>();
            for (const auto& p : member.projects)
                for (const auto& r : revisions[p])
                    if (r.at("revision_id") == id) return {200, r.dump()};
            return {404, "{}"};
        }
        return {404, "{}"};
    }

    // Head as the server tracks it: explicit after a publish through the RPC,
    // else derived from fixtures (highest published revision of the method).
    std::string headOf(const std::string& project, const std::string& methodId) {
        for (const auto& m : methods[project])
            if (m.methodId == methodId && !m.head.empty()) return m.head;
        std::string head;
        uint64_t best = 0;
        for (const auto& r : revisions[project])
            if (r.at("method_id") == methodId && r.at("state") == "published" &&
                r.at("revision_number").get<uint64_t>() > best) {
                best = r.at("revision_number").get<uint64_t>();
                head = r.at("revision_id").get<std::string>();
            }
        return head;
    }

private:
    static bool isMember(const User& u, const std::string& project) {
        return std::find(u.projects.begin(), u.projects.end(), project) != u.projects.end();
    }
    static bool hasRole(const User& u, const std::string& role) {
        return std::find(u.roles.begin(), u.roles.end(), role) != u.roles.end() ||
               std::find(u.roles.begin(), u.roles.end(), "admin") != u.roles.end();
    }
    std::vector<Method> methodsOf(const std::string& project) {
        std::vector<Method> out = methods[project];
        for (const auto& r : revisions[project]) {
            const auto id = r.at("method_id").get<std::string>();
            if (std::none_of(out.begin(), out.end(), [&](const Method& m) { return m.methodId == id; }))
                out.push_back({id, r.at("display_name").get<std::string>(), "", ""});
        }
        for (auto& m : out) m.head = headOf(project, m.methodId);
        return out;
    }
    std::string displayNameOf(const std::string& project, const std::string& methodId) {
        for (const auto& m : methodsOf(project))
            if (m.methodId == methodId) return m.displayName;
        return methodId;
    }
    void setHead(const std::string& project, const std::string& methodId, const std::string& head) {
        for (auto& m : methods[project])
            if (m.methodId == methodId) {
                m.head = head;
                return;
            }
        methods[project].push_back({methodId, displayNameOf(project, methodId), "", head});
    }
    Json* find(const std::string& id) {
        for (auto& [project, list] : revisions)
            for (auto& r : list)
                if (r.at("revision_id") == id) return &r;
        return nullptr;
    }
    void insertSorted(const std::string& project, const Json& r) {
        auto& list = revisions[project];
        const auto id = r.at("revision_id").get<std::string>();
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const Json& x) { return x.at("revision_id").get<std::string>() > id; });
        list.insert(it, r);
    }
    void event(const std::string& revisionId, const std::string& actor, const std::string& action,
               const std::string& reason) {
        events[revisionId].push_back({{"actor_id", actor}, {"action", action}, {"reason", reason},
                                      {"created_at", "2026-10-04T10:00:00Z"}});
    }
    const User& userById(const std::string& id) {
        for (const auto& [email, user] : users)
            if (user.id == id) return user;
        throw std::runtime_error("unknown user");
    }
    RegistryHttpResponse session(const std::string& email, const std::string& userId) {
        const auto access = "access-" + std::to_string(++tokenCounter);
        const auto refresh = "refresh-" + std::to_string(tokenCounter);
        accessTokens[access] = userId;
        refreshTokens[refresh] = userId;
        return {200, Json({{"access_token", access},
                           {"refresh_token", refresh},
                           {"token_type", "bearer"},
                           {"expires_in", expiresIn},
                           {"user", {{"id", userId}, {"email", email}}}})
                         .dump()};
    }
};

inline RegistryHttpTransport transportFor(FakeSupabase& fake) {
    return [&fake](const RegistryHttpRequest& request) { return fake.handle(request); };
}

} // namespace mib::test
