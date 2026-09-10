#include "backend/profiles/ProfileCache.h"
#include "backend/profiles/ProfileRegistryService.h"
#include <chrono>
#include "backend/profiles/SupabaseProfileRegistry.h"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

using namespace backend::profiles;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <class F> void rejects(F f, RegistryErrorCode code) {
    try {
        f();
    } catch (const RegistryError& error) {
        require(error.code == code, "wrong rejection code");
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}
Revision revision(std::string id = "r12", uint64_t number = 12) {
    Revision r;
    r.methodId = "method";
    r.revisionId = id;
    r.projectId = "project";
    r.authorId = "alice";
    r.revisionNumber = number;
    r.metadataVersion = 1;
    r.state = CentralState::Published;
    r.canonicalContent =
        canonicalMethod(R"({"config_schema_version":1,"gain":2})", "camera();", "core", 1);
    r.contentHash = contentHash(r.canonicalContent);
    return r;
}
nlohmann::json encode(const Revision& r) {
    return {{"method_id", r.methodId},
            {"revision_id", r.revisionId},
            {"project_id", r.projectId},
            {"parent_revision_id", nullptr},
            {"display_name", r.displayName},
            {"author_id", r.authorId},
            {"canonical_content", r.canonicalContent},
            {"content_hash", r.contentHash},
            {"revision_number", r.revisionNumber},
            {"metadata_version", r.metadataVersion},
            {"state", toString(r.state)}};
}
} // namespace
int main() {
    try {
        require(contentHash("abc") ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 golden vector");
        auto r = revision();
        require(r.canonicalContent == canonicalMethod("{\"gain\":2.0,\"config_schema_version\":1}",
                                                      "camera();", "core", 1),
                "canonical key/numeric stability");
        rejects(
            [] { canonicalMethod(R"({"config_schema_version":1,"a":1,"a":2})", "", "core", 1); },
            RegistryErrorCode::Invalid);
        rejects([] { canonicalMethod(R"({"config_schema_version":2})", "", "core", 1); },
                RegistryErrorCode::Invalid);
        auto damaged = r;
        damaged.canonicalContent += " ";
        rejects([&] { verifyRevision(damaged); }, RegistryErrorCode::Integrity);
        damaged.contentHash = contentHash(damaged.canonicalContent);
        rejects([&] { verifyRevision(damaged); }, RegistryErrorCode::Invalid);
        ProfileCache cache(":memory:", "https://registry.example", "bob");
        cache.store(r);
        cache.store(r);
        require(cache.read("r12").canonicalContent == r.canonicalContent, "cache roundtrip");
        require(!cache.eligibility("r12", "MIB-01", "ctx").eligible,
                "approval must not imply validation");
        LocalValidation v{"r12", "MIB-01", r.contentHash, "ctx", "bob", "fixture evidence", true};
        cache.recordValidation(v);
        require(cache.eligibility("r12", "MIB-01", "ctx").eligible, "eligible offline cache");
        require(!cache.eligibility("r12", "MIB-02", "ctx").eligible,
                "cross-instrument validation leak");
        require(!cache.eligibility("r12", "MIB-01", "new calibration").eligible,
                "stale context validation");
        auto replacement = r;
        replacement.canonicalContent =
            canonicalMethod(R"({"config_schema_version":1,"gain":3})", "", "core", 1);
        replacement.contentHash = contentHash(replacement.canonicalContent);
        rejects([&] { cache.store(replacement); }, RegistryErrorCode::Integrity);
        const auto frozen = cache.read("r12");
        auto next = revision("r13", 13);
        cache.store(next);
        require(frozen.revisionId == "r12" && frozen.contentHash == r.contentHash,
                "update mutated frozen value");
        cache.updateState("r12", CentralState::Superseded, 2);
        require(cache.eligibility("r12", "MIB-01", "ctx").eligible,
                "superseded revision should remain eligible");
        cache.updateState("r12", CentralState::Revoked, 3);
        cache.store(r);
        require(cache.read("r12").state == CentralState::Revoked, "stale sync undid revocation");
        require(!cache.eligibility("r12", "MIB-01", "ctx").eligible, "revoked execution");
        require(cache.read("r12").canonicalContent == frozen.canonicalContent,
                "revocation erased history");
        rejects([&] { cache.updateState("r12", CentralState::Published, 4); },
                RegistryErrorCode::Conflict);
        require(cache.read("r12").metadataVersion == 3, "failure did not roll back");
        unsigned status = 200;
        std::string response = encode(next).dump();
        SupabaseProfileRegistry provider(
            "https://registry.example", "sb_publishable_test", [] { return "user-token"; },
            [&](const RegistryHttpRequest& request) {
                require(request.timeoutMs == 8000 && request.maxResponseBytes == 2097152,
                        "unbounded request");
                require(request.headers.at("Authorization") == "Bearer user-token",
                        "user scoped authentication");
                return RegistryHttpResponse{status, response};
            });
        cache.store(provider.fetchRevision("r13"));
        ProfileRegistryService service(provider, cache);
        require(service.download("r13"), "service download");
        status = 503;
        require(!service.download("r13"), "service outage must be explicit");
        require(service.health().connectivity == RegistryHealth::Connectivity::Offline,
                "service health separate from instrument");
        rejects([&] { provider.fetchRevision("r13"); }, RegistryErrorCode::Offline);
        require(cache.read("r13").contentHash == next.contentHash, "outage altered cache");
        status = 401;
        rejects([&] { provider.fetchRevision("r13"); }, RegistryErrorCode::Authentication);
        status = 403;
        rejects([&] { provider.fetchRevision("r13"); }, RegistryErrorCode::Permission);
        status = 200;
        rejects([&] { provider.fetchRevision("r12"); }, RegistryErrorCode::Integrity);
        require(provider.fetchRevision("r13").revisionId == "r13", "reconnect fetch");
        response = nlohmann::json({{"revisions", nlohmann::json::array({encode(next)})},
                                   {"next_cursor", "r13"}})
                       .dump();
        require(service.syncPage("project").value() == "r13", "explicit page sync");
        require(!service.syncPage("project", "r13").has_value(), "repeated cursor rejected");
        require(!service.syncPage("another-project").has_value(),
                "cross-project response rejected");
        response =
            nlohmann::json({{"revisions", nlohmann::json::array()}, {"next_cursor", ""}}).dump();
        require(service.syncPage("project", "r13").value().empty(), "empty terminal page");
        response = "{}";
        rejects([&] { provider.fetchRevision("r13"); }, RegistryErrorCode::Invalid);
        rejects(
            [] {
                SupabaseProfileRegistry bad(
                    "http://registry.example", "sb_publishable_test", [] { return "x"; },
                    [](auto&) { return RegistryHttpResponse{}; });
            },
            RegistryErrorCode::Invalid);
        // Persistent reopen + wrong-account refusal + disk corruption, using an isolated directory.
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("mib-registry-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        const auto path = (dir / "cache.sqlite").string();
        {
            ProfileCache disk(path, "https://registry.example", "bob");
            disk.store(next);
        }
        {
            ProfileCache disk(path, "https://registry.example", "bob");
            require(disk.read("r13").contentHash == next.contentHash, "persistent reopen");
        }
        rejects([&] { ProfileCache disk(path, "https://registry.example", "alice"); },
                RegistryErrorCode::Permission);
        sqlite3* db = nullptr;
        require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "test sqlite open");
        require(sqlite3_exec(
                    db,
                    "DROP TRIGGER registry_immutable; UPDATE registry_revisions SET content='{}'",
                    nullptr, nullptr, nullptr) == SQLITE_OK,
                "inject corruption");
        sqlite3_close(db);
        {
            ProfileCache disk(path, "https://registry.example", "bob");
            rejects([&] { disk.read("r13"); }, RegistryErrorCode::Integrity);
        }
        std::filesystem::remove_all(dir);
        std::cout << "Profile registry integrity/offline/transport tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
