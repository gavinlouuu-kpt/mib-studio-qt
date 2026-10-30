#include "backend/database/SqliteService.h"
#include "support/tempdir.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <cstdlib>
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
    mib::test::TempDir temp("workspace_host");
    const std::string path = argc == 2 ? argv[1] : (temp.path() / "metadata.sqlite3").string();
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
    }
    return 0;
}
