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
    };

    std::mutex mutex;
    std::map<std::string, User> users;                  // email -> user
    std::map<std::string, std::string> accessTokens;    // token -> user id
    std::map<std::string, std::string> refreshTokens;   // token -> user id (single use)
    std::map<std::string, std::vector<Json>> revisions; // project -> sorted by id
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
                    {{"project_id", p}, {"display_name", "Project " + p}, {"roles", {"operator"}}});
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
        if (path == "/rest/v1/rpc/registry_fetch_revision") {
            const auto id = body.at("p_revision_id").get<std::string>();
            for (const auto& p : member.projects)
                for (const auto& r : revisions[p])
                    if (r.at("revision_id") == id) return {200, r.dump()};
            return {404, "{}"};
        }
        return {404, "{}"};
    }

private:
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
