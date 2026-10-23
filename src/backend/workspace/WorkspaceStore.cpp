#include "backend/workspace/WorkspaceStore.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <charconv>
#include <cstdint>
#include <limits>

namespace backend::workspace {
namespace {
void check(int code) {
    if (code != SQLITE_OK && code != SQLITE_ROW && code != SQLITE_DONE)
        throw StoreError(StoreErrorCode::Storage, "Workspace database operation failed");
}
void exec(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
void identifier(const std::string& id) {
    if (id.empty() || id.size() > 256 || id.front() == ' ' || id.back() == ' ')
        throw StoreError(StoreErrorCode::Invalid, "Invalid workspace identity");
    for (unsigned char c : id)
        if (c < 32 || c == 127)
            throw StoreError(StoreErrorCode::Invalid, "Invalid workspace identity");
}
std::int64_t revisionNumber(const std::string& value) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result <= 0 ||
        std::to_string(result) != value)
        throw StoreError(StoreErrorCode::Invalid, "Invalid workspace revision");
    return result;
}
std::string canonicalDocument(const std::string& input) {
    if (input.size() > 1024 * 1024)
        throw StoreError(StoreErrorCode::Invalid, "Workspace document exceeds 1 MiB");
    try {
        auto value = nlohmann::json::parse(
            input, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > 64)
                    throw StoreError(StoreErrorCode::Invalid, "Workspace nesting exceeds limit");
                return true;
            });
        if (!value.is_object() || !value.contains("schema_version") ||
            (!value["schema_version"].is_number_integer() || value["schema_version"] != 1))
            throw StoreError(StoreErrorCode::Invalid,
                             "Workspace document must be a version-1 object");
        return value.dump();
    } catch (const nlohmann::json::exception&) {
        throw StoreError(StoreErrorCode::Invalid, "Malformed workspace document");
    }
}
struct Statement {
    sqlite3_stmt* stmt{};
    Statement(sqlite3* db, const char* sql) {
        check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    Statement(const Statement&) = delete;
    void bind(int index, const std::string& value) {
        check(sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()),
                                SQLITE_TRANSIENT));
    }
    void bind(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt, index, value)); }
    int step() {
        const int result = sqlite3_step(stmt);
        check(result);
        return result;
    }
    std::string text(int index) const {
        const auto* data = sqlite3_column_text(stmt, index);
        return data ? std::string(reinterpret_cast<const char*>(data),
                                  sqlite3_column_bytes(stmt, index))
                    : "";
    }
};
struct Transaction {
    sqlite3* db;
    bool committed{false};
    explicit Transaction(sqlite3* connection) : db(connection) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        exec(db, "COMMIT");
        committed = true;
    }
};
} // namespace
WorkspaceStore::WorkspaceStore(sqlite3* connection) : db_(connection) {
    if (!db_) throw StoreError(StoreErrorCode::Invalid, "Workspace requires a metadata connection");
}
void WorkspaceStore::requireInitialized() const {
    if (!initialized_)
        throw StoreError(StoreErrorCode::Invalid, "Workspace schema is not initialized");
}
void WorkspaceStore::initialize() {
    initialized_ = false;
    Transaction tx(db_);
    exec(db_, "CREATE TABLE IF NOT EXISTS workspace_schema(version INTEGER NOT NULL PRIMARY KEY)");
    exec(db_, "INSERT OR IGNORE INTO workspace_schema(version) VALUES(1)");
    Statement version(db_, "SELECT version FROM workspace_schema");
    if (version.step() != SQLITE_ROW || version.text(0) != "1" || version.step() != SQLITE_DONE)
        throw StoreError(StoreErrorCode::Invalid, "Unsupported workspace schema");
    exec(db_, "CREATE TABLE IF NOT EXISTS workspace_revisions(workspace_id TEXT NOT NULL, revision "
              "INTEGER NOT NULL CHECK(revision>0), document_json TEXT NOT NULL, PRIMARY "
              "KEY(workspace_id,revision))");
    exec(db_, "CREATE TABLE IF NOT EXISTS workspace_commands(workspace_id TEXT NOT NULL, "
              "command_id TEXT NOT NULL, request_json TEXT NOT NULL, revision INTEGER NOT NULL, "
              "PRIMARY KEY(workspace_id,command_id))");
    tx.commit();
    initialized_ = true;
}
std::optional<Snapshot> WorkspaceStore::read(const std::string& id,
                                             const std::optional<std::string>& revision) const {
    requireInitialized();
    identifier(id);
    if (revision) revisionNumber(*revision);
    Statement query(db_, revision ? "SELECT revision,document_json FROM workspace_revisions WHERE "
                                    "workspace_id=? AND revision=?"
                                  : "SELECT revision,document_json FROM workspace_revisions WHERE "
                                    "workspace_id=? ORDER BY revision DESC LIMIT 1");
    query.bind(1, id);
    if (revision) query.bind(2, revisionNumber(*revision));
    if (query.step() == SQLITE_DONE) return std::nullopt;
    const auto number = query.text(0);
    revisionNumber(number);
    return Snapshot{id, number, canonicalDocument(query.text(1))};
}
std::vector<std::string> WorkspaceStore::history(const std::string& id, int limit,
                                                 const std::optional<std::string>& after) const {
    requireInitialized();
    identifier(id);
    if (limit < 1 || limit > 1000)
        throw StoreError(StoreErrorCode::Invalid, "History page limit must be 1..1000");
    const auto cursor = after ? revisionNumber(*after) : 0;
    Statement query(db_, "SELECT revision FROM workspace_revisions WHERE workspace_id=? AND "
                         "revision>? ORDER BY revision LIMIT ?");
    query.bind(1, id);
    query.bind(2, cursor);
    query.bind(3, static_cast<std::int64_t>(limit));
    std::vector<std::string> result;
    while (query.step() == SQLITE_ROW)
        result.push_back(query.text(0));
    return result;
}
std::vector<Snapshot> WorkspaceStore::catalog(const std::string& prefix, int limit,
                                              const std::string& after) const {
    requireInitialized();
    identifier(prefix);
    if (prefix.back() != '/' || limit < 1 || limit > 101 ||
        (!after.empty() && after.rfind(prefix, 0) != 0))
        throw StoreError(StoreErrorCode::Invalid, "Invalid catalog namespace, cursor or limit");
    if (!after.empty()) identifier(after);
    auto upper = prefix;
    upper.back() = '0'; // Exclusive binary bound immediately following the literal slash prefix.
    Statement query(db_, "SELECT workspace_id,MAX(revision) FROM workspace_revisions "
                         "WHERE workspace_id>=? AND workspace_id<? AND workspace_id>? "
                         "GROUP BY workspace_id ORDER BY workspace_id LIMIT ?");
    query.bind(1, prefix); query.bind(2, upper); query.bind(3, after);
    query.bind(4, static_cast<std::int64_t>(limit));
    std::vector<Snapshot> result;
    while (query.step() == SQLITE_ROW) result.push_back({query.text(0), query.text(1), {}});
    return result;
}
Snapshot WorkspaceStore::commit(const std::string& id, const std::optional<std::string>& expected,
                                const std::string& command, const std::string& document) {
    return write(id, expected, command, canonicalDocument(document), "commit");
}
std::optional<Snapshot> WorkspaceStore::receipt(const std::string& id,
    const std::optional<std::string>& expected, const std::string& command,
    const std::string& document) const {
    requireInitialized();
    identifier(id); identifier(command);
    if (expected) revisionNumber(*expected);
    const auto request = nlohmann::json{
        {"expected", expected ? nlohmann::json(*expected) : nlohmann::json(nullptr)},
        {"document", canonicalDocument(document)}, {"operation", "commit"}}.dump();
    Statement query(db_, "SELECT request_json,revision FROM workspace_commands WHERE workspace_id=? AND command_id=?");
    query.bind(1, id); query.bind(2, command);
    if (query.step() == SQLITE_DONE) return std::nullopt;
    if (query.text(0) != request)
        throw StoreError(StoreErrorCode::Invalid, "Command identity reused with different content");
    auto prior = read(id, query.text(1));
    if (!prior) throw StoreError(StoreErrorCode::Storage, "Command references a missing revision");
    return prior;
}
Snapshot WorkspaceStore::restore(const std::string& id, const std::string& expected,
                                 const std::string& command, const std::string& target) {
    const auto old = read(id, target);
    if (!old) throw StoreError(StoreErrorCode::NotFound, "Restore revision not found");
    return write(id, expected, command, old->documentJson, "restore:" + target);
}
Snapshot WorkspaceStore::write(const std::string& id, const std::optional<std::string>& expected,
                               const std::string& command, const std::string& document,
                               const std::string& operation) {
    requireInitialized();
    identifier(id);
    identifier(command);
    if (expected) revisionNumber(*expected);
    // Retain the full canonical request: command collisions cannot alias different payloads.
    const auto request = nlohmann::json{
        {"expected", expected ? nlohmann::json(*expected) : nlohmann::json(nullptr)},
        {"document", document},
        {"operation",
         operation}}.dump();
    Transaction tx(db_);
    {
        Statement replay(db_, "SELECT request_json,revision FROM workspace_commands WHERE "
                              "workspace_id=? AND command_id=?");
        replay.bind(1, id);
        replay.bind(2, command);
        if (replay.step() == SQLITE_ROW) {
            if (replay.text(0) != request)
                throw StoreError(StoreErrorCode::Invalid,
                                 "Command identity reused with different content");
            const auto prior = read(id, replay.text(1));
            if (!prior)
                throw StoreError(StoreErrorCode::Storage, "Command references a missing revision");
            tx.commit();
            return *prior;
        }
    }
    const auto current = read(id);
    if (expected && !current) throw StoreError(StoreErrorCode::NotFound, "Workspace not found");
    if ((!expected && current) || (expected && current->revision != *expected))
        throw StoreError(StoreErrorCode::Conflict,
                         "Workspace revision changed; reload before editing");
    const auto previous = current ? revisionNumber(current->revision) : 0;
    if (previous == std::numeric_limits<std::int64_t>::max())
        throw StoreError(StoreErrorCode::Invalid, "Workspace revision exhausted");
    const auto next = previous + 1;
    {
        Statement insert(
            db_,
            "INSERT INTO workspace_revisions(workspace_id,revision,document_json) VALUES(?,?,?)");
        insert.bind(1, id);
        insert.bind(2, next);
        insert.bind(3, document);
        insert.step();
        Statement log(
            db_, "INSERT INTO workspace_commands(workspace_id,command_id,request_json,revision) "
                 "VALUES(?,?,?,?)");
        log.bind(1, id);
        log.bind(2, command);
        log.bind(3, request);
        log.bind(4, next);
        log.step();
    }
    tx.commit();
    return Snapshot{id, std::to_string(next), document};
}
} // namespace backend::workspace
