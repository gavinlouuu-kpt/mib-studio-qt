#include "backend/workspace/WorkspaceStore.h"
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

using namespace backend::workspace;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void sql(sqlite3* db, const char* statement) {
    require(sqlite3_exec(db, statement, nullptr, nullptr, nullptr) == SQLITE_OK, "Test SQL failed");
}
struct Connection {
    sqlite3* db{};
    explicit Connection(const std::string& path) {
        require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Open failed");
        sqlite3_busy_timeout(db, 5000);
    }
    ~Connection() { sqlite3_close(db); }
};
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
template <typename F> void fails(StoreErrorCode code, F operation) {
    try {
        operation();
    } catch (const StoreError& error) {
        require(error.code == code, "Wrong error category");
        return;
    }
    throw std::runtime_error("Expected failure");
}
const std::string first = R"({"schema_version":1,"recipe":{"method":"r1","analysis":"p1"}})";
const std::string second = R"({"schema_version":1,"recipe":{"method":"r2","analysis":"p1"}})";
} // namespace
int main(int argc, char** argv) {
    Watchdog watchdog;
    try {
        require(argc <= 2, "Pass at most one fresh test database path");
        const std::string path = argc == 2 ? argv[1] :
            (std::filesystem::temp_directory_path() / ("ique-store-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite")).string();
        require(!std::filesystem::exists(path), "Refusing to overwrite an existing database");
        {
            Connection connection(path);
            WorkspaceStore store(connection.db);
            store.initialize();
            store.initialize();
            require(!store.read("study"), "Missing workspace must not become empty data");
            auto one = store.commit("study", std::nullopt, "create", first);
            require(one.revision == "1", "Create revision");
            auto two = store.commit("study", "1", "edit", second);
            require(two.revision == "2", "Edit revision");
            require(store.read("study", "1")->documentJson == one.documentJson,
                    "History immutable");
            require(store.commit("study", std::nullopt, "create", first).revision == "1",
                    "Lost reply replay after head advances");
            fails(StoreErrorCode::Invalid, [&] { store.commit("study", "1", "edit", first); });
            fails(StoreErrorCode::Conflict, [&] { store.commit("study", "1", "stale", first); });
            fails(StoreErrorCode::Conflict,
                  [&] { store.commit("study", std::nullopt, "duplicate", first); });
            fails(StoreErrorCode::NotFound, [&] { store.commit("missing", "1", "edit", first); });
            auto restored = store.restore("study", "2", "undo", "1");
            require(restored.revision == "3" && restored.documentJson == one.documentJson,
                    "Restore creates revision");
            require(store.restore("study", "2", "undo", "1").revision == "3", "Restore replay");
            require(store.history("study").size() == 3, "Restore keeps history");
            require(store.history("study", 1, "1") == std::vector<std::string>{"2"},
                    "History pagination");
            fails(StoreErrorCode::Invalid, [&] { store.history("study", 0); });
            fails(StoreErrorCode::Invalid,
                  [&] { store.commit("study", "3", "float-schema", R"({"schema_version":1.0})"); });
            fails(StoreErrorCode::NotFound, [&] { store.restore("study", "3", "bad-undo", "99"); });
            // Failure occurs after revision insertion, while publishing the command receipt.
            sql(connection.db, "CREATE TRIGGER fail_receipt BEFORE INSERT ON workspace_commands "
                               "BEGIN SELECT RAISE(ABORT,'injected failure'); END");
            fails(StoreErrorCode::Storage, [&] { store.commit("study", "3", "retry", second); });
            require(store.read("study")->revision == "3" && store.history("study").size() == 3,
                    "Failure must roll back the whole commit");
            sql(connection.db, "DROP TRIGGER fail_receipt");
            require(store.commit("study", "3", "retry", second).revision == "4",
                    "Failure leaves retry identity available");
            for (auto invalid : {"", "{}", "[]", "{broken", "{\"schema_version\":2}"})
                fails(StoreErrorCode::Invalid,
                      [&] { store.commit("study", "4", "invalid", invalid); });
            fails(StoreErrorCode::Invalid, [&] { store.read("study", "01"); });
            fails(StoreErrorCode::Invalid, [&] { store.commit("study", "4", "", first); });
            fails(StoreErrorCode::Invalid,
                  [&] { store.commit("study", "4", "large", std::string(1024 * 1024 + 1, 'x')); });
            store.commit("other-study", std::nullopt, "create", first);
            require(store.read("other-study")->revision == "1", "Workspace identity isolation");
        }
        {
            Connection connection(path);
            WorkspaceStore store(connection.db);
            store.initialize();
            require(store.read("study")->revision == "4", "Restart preserves head");
            require(store.commit("study", "3", "retry", second).revision == "4",
                    "Restart preserves command replay");
            // Host transactions remain host-owned: nested mutation must refuse without rolling them
            // back.
            sql(connection.db, "BEGIN");
            fails(StoreErrorCode::Storage, [&] { store.commit("study", "4", "nested", first); });
            require(!sqlite3_get_autocommit(connection.db),
                    "Store must not roll back host transaction");
            sql(connection.db, "ROLLBACK");
        }
        {
            Connection a(path), b(path);
            WorkspaceStore left(a.db), right(b.db);
            left.initialize();
            right.initialize();
            std::mutex mutex;
            std::condition_variable cv;
            bool start = false;
            std::atomic<int> success{0}, conflicts{0}, errors{0};
            auto race = [&](WorkspaceStore& store, const std::string& command) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&] { return start; });
                }
                try {
                    store.commit("study", "4", command, first);
                    ++success;
                } catch (const StoreError& error) {
                    if (error.code == StoreErrorCode::Conflict)
                        ++conflicts;
                    else
                        ++errors;
                } catch (...) {
                    ++errors;
                }
            };
            std::thread t1(race, std::ref(left), "left"), t2(race, std::ref(right), "right");
            {
                std::lock_guard<std::mutex> lock(mutex);
                start = true;
            }
            cv.notify_all();
            t1.join();
            t2.join();
            require(success == 1 && conflicts == 1 && errors == 0,
                    "Concurrent CAS must have exactly one winner");
            require(left.history("study").size() == 5, "Concurrent loser must publish nothing");
        }
        {
            Connection connection(path);
            WorkspaceStore store(connection.db);
            sql(connection.db, "UPDATE workspace_schema SET version=2");
            fails(StoreErrorCode::Invalid, [&] { store.initialize(); });
            fails(StoreErrorCode::Invalid, [&] { store.read("study"); });
            sqlite3_stmt* query{};
            require(sqlite3_prepare_v2(
                        connection.db,
                        "SELECT MAX(revision) FROM workspace_revisions WHERE workspace_id='study'",
                        -1, &query, nullptr) == SQLITE_OK,
                    "Inspect preserved data");
            require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 5,
                    "Unknown schema refusal preserves data");
            sqlite3_finalize(query);
        }
        std::cout << "Workspace persistence: round-trip, rollback, replay, restore, restart and "
                     "concurrent CAS passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
