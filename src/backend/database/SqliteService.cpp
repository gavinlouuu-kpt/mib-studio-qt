#include "backend/database/SqliteService.h"
#include "backend/workspace/WorkspaceStore.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <set>
#include <filesystem>

namespace backend::services {
using nlohmann::json;
using workspace::StoreError;
using workspace::StoreErrorCode;
SqliteService::SqliteService() = default;
SqliteService::~SqliteService() {
    workspace_.reset();
    if (db_) sqlite3_close(db_);
}
bool SqliteService::initialize(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Reinitialization must not silently move an existing metadata authority.
    if (db_) return false;
    sqlite3* connection = nullptr;
    if (sqlite3_open_v2(path.c_str(), &connection,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (connection) sqlite3_close(connection);
        return false;
    }
    try {
        sqlite3_busy_timeout(connection, 2000);
        if (sqlite3_exec(connection,
                         "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Metadata database configuration failed");
        auto store = std::make_unique<workspace::WorkspaceStore>(connection);
        store->initialize();
        db_ = connection;
        workspace_ = std::move(store);
        SPDLOG_INFO("App metadata database initialized");
        return true;
    } catch (...) {
        sqlite3_close(connection);
        SPDLOG_ERROR("App metadata database initialization failed");
        return false;
    }
}
bool SqliteService::execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ && sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}
std::string SqliteService::workspaceRequest(const std::string& request, bool write) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!workspace_)
        return R"({"status":"unavailable","message":"Metadata database is not available"})";
    try {
        if (request.size() > 1100000)
            throw StoreError(StoreErrorCode::Invalid, "Workspace request too large");
        const auto q = json::parse(request, [](int depth, json::parse_event_t, json&) {
            if (depth > 68)
                throw StoreError(StoreErrorCode::Invalid,
                                 "Workspace request nesting exceeds limit");
            return true;
        });
        if (!q.is_object())
            throw StoreError(StoreErrorCode::Invalid, "Expected workspace request object");
        const auto action = q.at("action").get<std::string>();
        std::set<std::string> keys{"action", "workspace_id"};
        if (action == "list") {
            keys.insert("after");
            keys.insert("limit");
        } else if (action == "export_status") {
            keys.insert("job_id");
        } else if (write && action == "export") {
            keys.insert("job_id");
            keys.insert("revision");
        } else if (write && action == "import") {
            keys.insert("command_id");
            keys.insert("manifest");
        } else if (write && (action == "attach_source" || action == "relink")) {
            keys.insert("command_id");
            keys.insert("expected_revision");
            keys.insert("source_id");
            keys.insert("path");
        } else if (action == "verify_source") {
            keys.insert("revision");
            keys.insert("source_id");
        } else if (action == "read")
            keys.insert("revision");
        else if (action == "history") {
            keys.insert("after");
            keys.insert("limit");
        } else if ((write && action == "commit") || action == "receipt") {
            keys.insert("expected_revision");
            keys.insert("command_id");
            keys.insert("document");
        } else if (write && action == "restore") {
            keys.insert("expected_revision");
            keys.insert("command_id");
            keys.insert("target_revision");
        } else
            throw StoreError(StoreErrorCode::Invalid,
                             "Unsupported workspace action on this endpoint");
        for (auto it = q.begin(); it != q.end(); ++it)
            if (!keys.count(it.key()))
                throw StoreError(StoreErrorCode::Invalid, "Unknown workspace request field");
        const auto id = action == "list" ? std::string{} : q.at("workspace_id").get<std::string>();
        const auto optional = [&](const char* key) -> std::optional<std::string> {
            if (!q.contains(key) || q.at(key).is_null()) return std::nullopt;
            return q.at(key).get<std::string>();
        };
        const auto snapshot = [](const workspace::Snapshot& s) {
            return json{{"status", "ok"},
                        {"workspace_id", s.workspaceId},
                        {"revision", s.revision},
                        {"document", json::parse(s.documentJson)}};
        };
        if (action == "list") {
            auto rows = json::array();
            for (const auto& s :
                 workspace_->list(q.value("limit", 100), q.value("after", std::string{}))) {
                const auto d = json::parse(s.documentJson);
                rows.push_back(
                    {{"workspace_id", s.workspaceId},
                     {"revision", s.revision},
                     {"title", d.value("title", s.workspaceId)},
                     {"archived", d.value("archived", false)},
                     {"kind", d.value("type", d.contains("plan") ? std::string{"workspace"}
                                                                 : std::string{"document"})}});
            }
            return json{{"status", "ok"}, {"workspaces", rows}}.dump();
        }
        if (action == "export")
            return workspace_->exportSnapshot(id, q.at("revision").get<std::string>(),
                                              q.at("job_id").get<std::string>());
        if (action == "export_status")
            return workspace_->exportStatus(id, q.at("job_id").get<std::string>());
        if (action == "import")
            return snapshot(workspace_->importSnapshot(id, q.at("command_id").get<std::string>(),
                                                       q.at("manifest").dump()))
                .dump();
        if (action == "attach_source" || action == "relink")
            return snapshot(workspace_->linkSource(id, q.at("expected_revision").get<std::string>(),
                                                   q.at("command_id").get<std::string>(),
                                                   q.at("source_id").get<std::string>(),
                                                   q.at("path").get<std::string>(),
                                                   action == "relink"))
                .dump();
        if (action == "verify_source")
            return workspace_->verifySource(id, q.at("revision").get<std::string>(),
                                            q.at("source_id").get<std::string>());
        if (action == "read") {
            auto s = workspace_->read(id, optional("revision"));
            return s ? snapshot(*s).dump()
                     : R"({"status":"not_found","message":"Workspace revision not found"})";
        }
        if (action == "history")
            return json{
                {"status", "ok"},
                {"revisions", workspace_->history(id, q.value("limit", 100), optional("after"))}}
                .dump();
        if (action == "receipt") {
            q.at("expected_revision");
            const auto saved =
                workspace_->receipt(id, optional("expected_revision"),
                                    q.at("command_id").get<std::string>(), q.at("document").dump());
            return saved ? snapshot(*saved).dump()
                         : R"({"status":"not_found","message":"No successful command receipt"})";
        }
        if (action == "commit") {
            // Missing CAS is a caller bug; explicit null means create-only.
            q.at("expected_revision");
            return snapshot(workspace_->commit(id, optional("expected_revision"),
                                               q.at("command_id").get<std::string>(),
                                               q.at("document").dump()))
                .dump();
        }
        return snapshot(workspace_->restore(id, q.at("expected_revision").get<std::string>(),
                                            q.at("command_id").get<std::string>(),
                                            q.at("target_revision").get<std::string>()))
            .dump();
    } catch (const StoreError& e) {
        const char* code = e.code == StoreErrorCode::Conflict   ? "conflict"
                           : e.code == StoreErrorCode::NotFound ? "not_found"
                           : e.code == StoreErrorCode::Invalid  ? "invalid"
                                                                : "storage";
        return json{{"status", code}, {"message", e.what()}}.dump();
    } catch (const std::filesystem::filesystem_error&) {
        return R"({"status":"not_found","message":"Source file unavailable; no workspace change was committed"})";
    } catch (const json::exception&) {
        return R"({"status":"invalid","message":"Malformed workspace request"})";
    } catch (...) {
        return R"({"status":"storage","message":"Metadata operation failed; retry the same command to reconcile"})";
    }
}
} // namespace backend::services
