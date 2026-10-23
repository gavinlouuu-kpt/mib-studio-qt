#include "backend/database/SqliteService.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <fstream>
using nlohmann::json;
void check(bool b) {
    if (!b) throw std::runtime_error("Workspace host assertion failed");
}
struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    std::thread thread;
    Watchdog()
        : thread([this] {
              std::unique_lock<std::mutex> lock(mutex);
              if (!cv.wait_for(lock, std::chrono::seconds(30), [this] { return done; }))
                  std::_Exit(99);
          }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_all();
        thread.join();
    }
};

int main(int argc, char** argv) {
    Watchdog watchdog;
    const std::string path =
        argc == 2
            ? argv[1]
            : "workspace-host-test-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  ".sqlite";
    check(!std::filesystem::exists(path));
    const json commit = {{"action", "commit"},
                         {"workspace_id", "test"},
                         {"expected_revision", nullptr},
                         {"command_id", "one"},
                         {"document", {{"schema_version", 1}, {"notes", "first"}}}};
    const json read = {{"action", "read"}, {"workspace_id", "test"}};
    {
        backend::services::SqliteService host;
        check(json::parse(host.workspaceRequest(read.dump(), false))["status"] == "unavailable");
        check(host.initialize(path));
        check(!host.initialize(":memory:"));
        // Two callers contend on the host-owned connection, not separate test connections.
        auto race = commit;
        race["workspace_id"] = "race";
        std::string left, right;
        std::thread one([&] { left = host.workspaceRequest(race.dump(), true); });
        std::thread two([&] {
            auto other = race;
            other["command_id"] = "other";
            right = host.workspaceRequest(other.dump(), true);
        });
        one.join();
        two.join();
        const auto l = json::parse(left)["status"], r = json::parse(right)["status"];
        check((l == "ok" && r == "conflict") || (l == "conflict" && r == "ok"));

        check(json::parse(host.workspaceRequest(commit.dump(), false))["status"] == "invalid");
        check(json::parse(host.workspaceRequest(read.dump(), false))["status"] == "not_found");
        auto first = json::parse(host.workspaceRequest(commit.dump(), true));
        check(first["revision"] == "1");
        check(json::parse(host.workspaceRequest(commit.dump(), true)) == first);
        auto next = commit;
        next["command_id"] = "two";
        next["expected_revision"] = "1";
        next["document"]["notes"] = "second";
        check(json::parse(host.workspaceRequest(next.dump(), true))["revision"] == "2");
        next["command_id"] = "stale";
        check(json::parse(host.workspaceRequest(next.dump(), true))["status"] == "conflict");
        next.erase("expected_revision");
        check(json::parse(host.workspaceRequest(next.dump(), true))["status"] == "invalid");
        // Fail receipt insertion after revision insertion: neither may survive.
        check(host.execute("CREATE TRIGGER fail_receipt BEFORE INSERT ON workspace_commands BEGIN "
                           "SELECT RAISE(ABORT,'injected'); END"));
        next["expected_revision"] = "2";
        check(json::parse(host.workspaceRequest(next.dump(), true))["status"] == "storage");
        check(json::parse(host.workspaceRequest(read.dump(), false))["revision"] == "2");
        check(host.execute("DROP TRIGGER fail_receipt"));
        check(json::parse(host.workspaceRequest(next.dump(), true))["revision"] == "3");
    }
    {
        backend::services::SqliteService host;
        check(host.initialize(path));
        check(json::parse(host.workspaceRequest(read.dump(), false))["revision"] == "3");
        check(json::parse(host.workspaceRequest(commit.dump(), true))["revision"] == "1");
        const json restore = {{"action", "restore"},
                              {"workspace_id", "test"},
                              {"expected_revision", "3"},
                              {"command_id", "restore"},
                              {"target_revision", "1"}};
        auto result = json::parse(host.workspaceRequest(restore.dump(), true));
        check(result["revision"] == "4" && result["document"]["notes"] == "first");
        check(json::parse(host.workspaceRequest("{", true))["status"] == "invalid");
        auto invoke = [&](json q, bool write = true) {
            return json::parse(host.workspaceRequest(q.dump(), write));
        };
        const json exportRequest = {{"action", "export"},
                                    {"workspace_id", "test"},
                                    {"revision", "4"},
                                    {"job_id", "export-one"}};
        check(invoke(exportRequest, false)["status"] == "invalid");
        const auto exported = invoke(exportRequest);
        check(exported["state"] == "complete");
        check(invoke(exportRequest) == exported);
        auto collision = exportRequest;
        collision["revision"] = "1";
        check(invoke(collision)["status"] == "invalid");
        const json imported = {{"action", "import"},
                               {"workspace_id", "imported"},
                               {"command_id", "import-one"},
                               {"manifest", exported.at("manifest")}};
        check(invoke(imported)["revision"] == "1");
        check(invoke(imported)["revision"] == "1");
        auto damaged = imported;
        damaged["workspace_id"] = "damaged";
        damaged["manifest"]["payload"]["document"]["notes"] = "corrupt";
        check(invoke(damaged)["status"] == "invalid");
        auto overwrite = imported;
        overwrite["command_id"] = "overwrite";
        check(invoke(overwrite)["status"] == "conflict");
        check(invoke({{"action", "list"}, {"limit", 1}}, false)["workspaces"].size() == 1);
        check(
            invoke({{"action", "list"}, {"limit", 100}, {"after", "imported"}}, false)["workspaces"]
                .size() == 2);
        const auto original = path + ".source", moved = path + ".moved", wrong = path + ".wrong";
        {
            std::ofstream f(original, std::ios::binary);
            f << "immutable-source";
        }
        {
            std::ofstream f(wrong, std::ios::binary);
            f << "different-source";
        }
        const json attach = {{"action", "attach_source"}, {"workspace_id", "imported"},
                             {"expected_revision", "1"},  {"command_id", "attach"},
                             {"source_id", "run-a"},      {"path", original}};
        check(invoke(attach)["revision"] == "2");
        std::filesystem::rename(original, moved);
        // An acknowledged attach must replay even when its old path no longer exists.
        check(invoke(attach)["revision"] == "2");
        auto relink = attach;
        relink["action"] = "relink";
        relink["expected_revision"] = "2";
        relink["command_id"] = "move";
        relink["path"] = wrong;
        check(invoke(relink)["status"] == "conflict");
        relink["path"] = moved;
        check(invoke(relink)["revision"] == "3");
        check(invoke(relink)["revision"] == "3");
        const json verify = {{"action", "verify_source"},
                             {"workspace_id", "imported"},
                             {"revision", "3"},
                             {"source_id", "run-a"}};
        check(invoke(verify, false)["verified"] == true);
        {
            std::ofstream f(moved, std::ios::binary);
            f << "changed-source";
        }
        check(invoke(verify, false)["status"] == "conflict");
        check(invoke(relink)["revision"] == "3"); // successful receipt remains immutable
        std::filesystem::remove(moved);
        std::filesystem::remove(wrong);
    }
    {
        backend::services::SqliteService host;
        check(host.initialize(path));
        auto exported = json::parse(host.workspaceRequest(
            R"({"action":"export_status","workspace_id":"test","job_id":"export-one"})", false));
        check(exported["state"] == "complete" && exported["revision"] == "4");
        check(exported["manifest"]["payload"]["document"]["notes"] == "first");
    }
    {
        backend::services::SqliteService source;
        check(source.initialize(path + ".definitions"));
        auto call = [&](backend::services::SqliteService& db, json q, bool write = true) {
            return json::parse(db.analysisWorkspaceRequest(q.dump(), write));
        };
        const json strategy = {
            {"schema_version", 1}, {"type", "F-analysis-strategy/1"}, {"graph", {{"id", "p1"}}}};
        check(call(source, {{"action", "commit"},
                            {"workspace_id", "analysis/p1"},
                            {"expected_revision", nullptr},
                            {"command_id", "plan"},
                            {"document", strategy}})["status"] == "ok");
        check(call(source, {{"action", "commit"},
                            {"workspace_id", "analysis/p1"},
                            {"expected_revision", "1"},
                            {"command_id", "plan2"},
                            {"document", strategy}})["revision"] == "2");
        const json document = {
            {"schema_version", 1},
            {"findings", {{{"recipe", {{"analysis_plan", {{"id", "p1"}, {"revision", "2"}}}}}}}},
            {"recipe", {{"analysis_plan", {{"id", "p1"}, {"revision", "1"}}}}}};
        check(call(source, {{"action", "commit"},
                            {"workspace_id", "dependent"},
                            {"expected_revision", nullptr},
                            {"command_id", "workspace"},
                            {"document", document}})["status"] == "ok");
        const auto exported = call(source, {{"action", "export"},
                                            {"workspace_id", "dependent"},
                                            {"revision", "1"},
                                            {"job_id", "export"}});
        check(exported["manifest"]["payload"]["dependencies"].size() == 2);
        const json import = {{"action", "import"},
                             {"workspace_id", "imported-dependent"},
                             {"command_id", "import"},
                             {"manifest", exported["manifest"]}};
        backend::services::SqliteService target;
        check(target.initialize(path + ".transfer"));
        check(json::parse(target.workspaceRequest(import.dump(), true))["status"] == "invalid");
        for (const auto& prefix :
             {"analysis/", "analysis-source/", "analysis-finding/", "analysis-report/"}) {
            const json spoof = {{"action", "commit"},
                                {"workspace_id", std::string(prefix) + "spoof"},
                                {"expected_revision", nullptr},
                                {"command_id", "spoof"},
                                {"document", strategy}};
            check(json::parse(target.workspaceRequest(spoof.dump(), true))["status"] == "invalid");
        }
        check(target.execute(
            "CREATE TRIGGER fail_dependency BEFORE INSERT ON workspace_revisions WHEN "
            "NEW.workspace_id='analysis/p1' BEGIN SELECT RAISE(ABORT,'dependency failure'); END"));
        check(call(target, import)["status"] == "storage");
        check(call(target, {{"action", "read"}, {"workspace_id", "imported-dependent"}},
                   false)["status"] == "not_found");
        check(target.execute("DROP TRIGGER fail_dependency"));
        check(call(target, import)["revision"] == "1");
        check(json::parse(target.workspaceRequest(import.dump(), true))["revision"] == "1");
        check(call(target, {{"action", "read"}, {"workspace_id", "analysis/p1"}, {"revision", "1"}},
                   false)["document"] == strategy);
        backend::services::SqliteService conflict;
        check(conflict.initialize(path + ".conflict"));
        auto other = strategy;
        other["graph"]["id"] = "other";
        check(call(conflict, {{"action", "commit"},
                              {"workspace_id", "analysis/p1"},
                              {"expected_revision", nullptr},
                              {"command_id", "other"},
                              {"document", other}})["status"] == "ok");
        check(call(conflict, import)["status"] == "conflict");
        check(call(conflict, {{"action", "read"}, {"workspace_id", "imported-dependent"}},
                   false)["status"] == "not_found");
    }
    return 0;
}
