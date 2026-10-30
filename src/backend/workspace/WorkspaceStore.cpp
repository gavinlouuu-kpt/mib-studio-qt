#include "backend/workspace/WorkspaceStore.h"
#include <sqlite3.h>
#include "backend/processing/ProcessingCoreSha256.h"
#include <nlohmann/json.hpp>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <filesystem>
#include <chrono>

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
    exec(db_, "CREATE TABLE IF NOT EXISTS workspace_exports(workspace_id TEXT NOT NULL, "
              "job_id TEXT NOT NULL, revision TEXT NOT NULL, result_json TEXT NOT NULL, "
              "PRIMARY KEY(workspace_id,job_id))");
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
std::vector<Snapshot> WorkspaceStore::list(int limit, const std::string& after) const {
    requireInitialized();
    if (limit < 1 || limit > 100)
        throw StoreError(StoreErrorCode::Invalid, "List limit must be 1..100");
    if (!after.empty()) identifier(after);
    Statement q(
        db_, "SELECT r.workspace_id,r.revision,r.document_json FROM workspace_revisions r "
             "JOIN (SELECT workspace_id,MAX(revision) revision FROM workspace_revisions "
             "GROUP BY workspace_id) h ON r.workspace_id=h.workspace_id AND r.revision=h.revision "
             "WHERE r.workspace_id>? ORDER BY r.workspace_id LIMIT ?");
    q.bind(1, after);
    q.bind(2, static_cast<std::int64_t>(limit));
    std::vector<Snapshot> rows;
    while (q.step() == SQLITE_ROW)
        rows.push_back({q.text(0), q.text(1), q.text(2)});
    return rows;
}
namespace {
std::set<std::pair<std::string, std::string>> analysisReferences(const nlohmann::json& doc) {
    std::set<std::pair<std::string, std::string>> refs;
    auto recipe = [&](const nlohmann::json& r) {
        const auto& p = r.at("analysis_plan");
        const auto id = p.at("id").get<std::string>(), rev = p.at("revision").get<std::string>();
        identifier("analysis/" + id);
        identifier(rev);
        refs.insert({"analysis/" + id, rev});
    };
    if (doc.contains("recipe")) recipe(doc.at("recipe"));
    if (doc.contains("findings") && doc.at("findings").is_array())
        for (const auto& f : doc.at("findings"))
            if (f.is_object() && f.contains("recipe")) recipe(f.at("recipe"));
    if (doc.contains("next_run_evidence")) {
        const auto& f = doc.at("next_run_evidence").at("finding");
        refs.insert({"analysis-finding/" + f.at("findingId").get<std::string>(),
                     f.at("revision").get<std::string>()});
    }
    if (doc.value("type", std::string{}) == "F-analysis-finding/1") {
        for (const auto& sample : doc.at("request").at("spec").at("samples")) {
            refs.insert({"analysis/" + sample.at("planId").get<std::string>(),
                         sample.at("revision").get<std::string>()});
            if (sample.at("source").at("kind") != "retained")
                throw StoreError(StoreErrorCode::Invalid,
                                 "Finding requires retained native sources");
            refs.insert(
                {"analysis-source/" + sample.at("source").at("id").get<std::string>(), "1"});
        }
    }
    if (doc.value("type", std::string{}) == "F-analysis-strategy/1" && doc.contains("application")) {
        const auto& a = doc.at("application");
        refs.insert(
            {"analysis/" + a.at("planId").get<std::string>(), a.at("revision").get<std::string>()});
    }
    if (refs.size() > 64)
        throw StoreError(StoreErrorCode::Invalid, "Too many pinned analysis dependencies");
    return refs;
}
std::string digest(const std::string& bytes) {
    return processing::processingCoreBytesSha256(reinterpret_cast<const uint8_t*>(bytes.data()),
                                                 bytes.size());
}
} // namespace
std::string WorkspaceStore::exportStatus(const std::string& id, const std::string& job) const {
    requireInitialized();
    identifier(id);
    identifier(job);
    Statement q(db_, "SELECT result_json FROM workspace_exports WHERE workspace_id=? AND job_id=?");
    q.bind(1, id);
    q.bind(2, job);
    if (q.step() != SQLITE_ROW) throw StoreError(StoreErrorCode::NotFound, "Export job not found");
    return q.text(0);
}
std::string WorkspaceStore::exportSnapshot(const std::string& id, const std::string& revision,
                                           const std::string& job) {
    requireInitialized();
    identifier(id);
    identifier(job);
    revisionNumber(revision);
    Transaction tx(db_);
    Statement prior(
        db_,
        "SELECT revision,result_json FROM workspace_exports WHERE workspace_id=? AND job_id=?");
    prior.bind(1, id);
    prior.bind(2, job);
    if (prior.step() == SQLITE_ROW) {
        if (prior.text(0) != revision)
            throw StoreError(StoreErrorCode::Invalid, "Export job identity reused");
        const auto result = prior.text(1);
        tx.commit();
        return result;
    }
    const auto saved = read(id, revision);
    if (!saved) throw StoreError(StoreErrorCode::NotFound, "Export revision not found");
    nlohmann::json payload = {{"format", "I-workspace/1"},
                              {"workspace_id", id},
                              {"revision", revision},
                              {"document", nlohmann::json::parse(saved->documentJson)}};
    payload["dependencies"] = nlohmann::json::array();
    payload["unresolved_references"] = nlohmann::json::array();
    auto pendingRefs = analysisReferences(payload.at("document"));
    std::set<std::pair<std::string, std::string>> visited;
    while (!pendingRefs.empty()) {
        const auto [dependencyId, dependencyRevision] = *pendingRefs.begin();
        pendingRefs.erase(pendingRefs.begin());
        if (!visited.insert({dependencyId, dependencyRevision}).second) continue;
        if (visited.size() > 64) throw StoreError(StoreErrorCode::Invalid, "Too many dependencies");
        std::optional<Snapshot> pinned;
        try {
            pinned = read(dependencyId, dependencyRevision);
        } catch (const StoreError& e) {
            if (e.code != StoreErrorCode::Invalid) throw;
        }
        if (pinned) {
            const auto nested = analysisReferences(nlohmann::json::parse(pinned->documentJson));
            pendingRefs.insert(nested.begin(), nested.end());
            payload["dependencies"].push_back(
                {{"workspace_id", dependencyId},
                 {"revision", dependencyRevision},
                 {"document", nlohmann::json::parse(pinned->documentJson)}});
        } else
            payload["unresolved_references"].push_back(
                {{"workspace_id", dependencyId}, {"revision", dependencyRevision}});
    }
    if (payload.dump().size() > 1024 * 1024)
        throw StoreError(StoreErrorCode::Invalid, "Export with dependencies exceeds 1 MiB");
    nlohmann::json manifest = {{"payload", payload}, {"sha256", digest(payload.dump())}};
    const auto result = nlohmann::json{
        {"status", "ok"},
        {"state", "complete"},
        {"job_id", job},
        {"workspace_id", id},
        {"revision", revision},
        {"manifest", manifest},
        {"manifest_json",
         manifest.dump()}}.dump();
    Statement insert(
        db_,
        "INSERT INTO workspace_exports(workspace_id,job_id,revision,result_json) VALUES(?,?,?,?)");
    insert.bind(1, id);
    insert.bind(2, job);
    insert.bind(3, revision);
    insert.bind(4, result);
    insert.step();
    tx.commit();
    return result;
}
Snapshot WorkspaceStore::importSnapshot(const std::string& id, const std::string& command,
                                        const std::string& text) {
    if (text.size() > 1100000)
        throw StoreError(StoreErrorCode::Invalid, "Import exceeds size limit");
    const auto manifest = nlohmann::json::parse(text);
    const auto& payload = manifest.at("payload");
    if (manifest.size() != 2 || !payload.is_object() ||
        (payload.size() != 4 && payload.size() != 6) || payload.at("format") != "I-workspace/1" ||
        manifest.at("sha256") != digest(payload.dump()))
        throw StoreError(StoreErrorCode::Invalid, "Unsupported or damaged workspace export");
    identifier(payload.at("workspace_id").get<std::string>());
    revisionNumber(payload.at("revision").get<std::string>());
    const auto document = canonicalDocument(payload.at("document").dump());
    auto refs = analysisReferences(payload.at("document"));
    if (payload.contains("dependencies")) {
        // Compute transitive closure only from reachable documents, never unrelated entries.
        for (std::size_t pass = 0; pass < 64; ++pass) {
            const auto previous = refs.size();
            for (const auto& dep : payload.at("dependencies")) {
                if (refs.count({dep.at("workspace_id").get<std::string>(),
                                dep.at("revision").get<std::string>()})) {
                    const auto nested = analysisReferences(dep.at("document"));
                    refs.insert(nested.begin(), nested.end());
                }
            }
            if (refs.size() > 64)
                throw StoreError(StoreErrorCode::Invalid, "Too many dependencies");
            if (refs.size() == previous) break;
        }
    }
    if (payload.at("document").contains("next_run_evidence")) {
        // A portable next-run draft cannot silently discard its scientific authority.
        std::set<std::pair<std::string, std::string>> checked;
        auto pending = refs;
        while (!pending.empty()) {
            const auto key = *pending.begin();
            pending.erase(pending.begin());
            if (!checked.insert(key).second) continue;
            if (checked.size() > 64)
                throw StoreError(StoreErrorCode::Invalid, "Too many dependencies");
            const auto native = read(key.first, key.second);
            if (!native)
                throw StoreError(StoreErrorCode::Invalid, "Next-run native dependency unavailable");
            const auto nested = analysisReferences(nlohmann::json::parse(native->documentJson));
            pending.insert(nested.begin(), nested.end());
        }
        refs.insert(checked.begin(), checked.end());
        const auto& evidence = payload.at("document").at("next_run_evidence");
        const auto& findingRef = evidence.at("finding");
        const auto finding = nlohmann::json::parse(
            read("analysis-finding/" + findingRef.at("findingId").get<std::string>(),
                 findingRef.at("revision").get<std::string>())
                ->documentJson);
        const auto& planRef = evidence.at("analysisPlan");
        const auto strategy = read("analysis/" + planRef.at("id").get<std::string>(),
                                   planRef.at("revision").get<std::string>());
        const auto& recipe = payload.at("document").at("recipe");
        if (evidence.at("schema") != "I-next-run-evidence/1" || evidence.at("applied") != false ||
            evidence.at("compatibility") != "unknown" ||
            evidence.at("hardwareReadiness") != "not_evaluated" ||
            recipe.at("method").at("id") != evidence.at("method").at("id") ||
            recipe.at("method").at("revision") != evidence.at("method").at("revision") ||
            planRef.at("id") != evidence.at("sample").at("reference").at("planId") ||
            planRef.at("revision") != evidence.at("sample").at("reference").at("revision"))
            throw StoreError(StoreErrorCode::Invalid, "Next-run recipe/provenance mismatch");
        if (!strategy || evidence.at("findingDocumentHash") != "sha256:" + digest(finding.dump()) ||
            findingRef.at("contentHash") != finding.at("contentHash") ||
            finding.at("contentHash") != "sha256:" + digest(finding.at("comparison").dump()) ||
            evidence.at("sample") != finding.at("comparison")
                                         .at("samples")
                                         .at(evidence.at("sampleIndex").get<std::size_t>()) ||
            planRef.at("documentHash") !=
                "sha256:" + digest(nlohmann::json::parse(strategy->documentJson).dump()) ||
            payload.at("document").at("recipe").at("analysis_plan").at("id") != planRef.at("id") ||
            payload.at("document").at("recipe").at("analysis_plan").at("revision") !=
                planRef.at("revision"))
            throw StoreError(StoreErrorCode::Invalid,
                             "Next-run evidence differs from native authority");
    }
    Transaction tx(db_);
    auto result = writeInTransaction(id, std::nullopt, command, document,
                                     "import:" + manifest.at("sha256").get<std::string>());
    if (payload.size() == 6) {
        const auto& dependencies = payload.at("dependencies");
        if (!dependencies.is_array() || dependencies.size() > 64 ||
            !payload.at("unresolved_references").is_array())
            throw StoreError(StoreErrorCode::Invalid, "Invalid export dependency list");
        std::set<std::pair<std::string, std::string>> seen;
        std::set<std::string> createdIdentities;
        for (const auto& dep : dependencies) {
            if (!dep.is_object() || dep.size() != 3)
                throw StoreError(StoreErrorCode::Invalid, "Invalid dependency envelope");
            const auto did = dep.at("workspace_id").get<std::string>(),
                       rev = dep.at("revision").get<std::string>();
            if (!refs.count({did, rev}) || !seen.insert({did, rev}).second ||
                !((did.rfind("analysis/", 0) == 0 &&
                   dep.at("document").value("type", std::string{}) == "F-analysis-strategy/1") ||
                  (did.rfind("analysis-finding/", 0) == 0 && rev == "1" &&
                   dep.at("document").value("type", std::string{}) == "F-analysis-finding/1") ||
                  (did.rfind("analysis-source/", 0) == 0 && rev == "1" &&
                   dep.at("document").value("type", std::string{}) == "F-retained-source/1")))
                throw StoreError(StoreErrorCode::Invalid,
                                 "Unreferenced or invalid analysis dependency");
            const auto content = canonicalDocument(dep.at("document").dump());
            const auto existing = read(did, rev);
            if (existing) {
                if (existing->documentJson != content)
                    throw StoreError(StoreErrorCode::Conflict,
                                     "Pinned analysis dependency differs; import rolled back");
            } else {
                if (read(did) && !createdIdentities.count(did))
                    throw StoreError(StoreErrorCode::Conflict,
                                     "Analysis identity already exists without this revision; "
                                     "import rolled back");
                Statement insert(
                    db_, "INSERT INTO workspace_revisions(workspace_id,revision,document_json) "
                         "VALUES(?,?,?)");
                insert.bind(1, did);
                insert.bind(2, revisionNumber(rev));
                insert.bind(3, content);
                insert.step();
                createdIdentities.insert(did);
            }
        }
    }
    tx.commit();
    return result;
}
namespace {
nlohmann::json fingerprint(const std::string& input) {
    namespace fs = std::filesystem;
    if (input.empty() || input.find('\0') != std::string::npos)
        throw StoreError(StoreErrorCode::Invalid, "Invalid source path");
    const auto path = fs::canonical(input);
    if (!fs::is_regular_file(path))
        throw StoreError(StoreErrorCode::Invalid, "Source must be a regular file");
    const auto size = fs::file_size(path);
    const auto modified = fs::last_write_time(path);
    if (size > 2ULL * 1024 * 1024 * 1024)
        throw StoreError(StoreErrorCode::Invalid, "Source verification limit is 2 GiB");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string why;
    const auto sha = processing::fileSha256(
        path, &why, [&] { return std::chrono::steady_clock::now() > deadline; });
    if (sha.empty()) throw StoreError(StoreErrorCode::Storage, "Source fingerprint failed: " + why);
    if (fs::canonical(input) != path || fs::file_size(path) != size ||
        fs::last_write_time(path) != modified)
        throw StoreError(StoreErrorCode::Conflict, "Source changed during verification");
    return {{"path", path.string()}, {"sha256", sha}, {"size_bytes", std::to_string(size)}};
}
} // namespace
Snapshot WorkspaceStore::linkSource(const std::string& id, const std::string& expected,
                                    const std::string& command, const std::string& sourceId,
                                    const std::string& path, bool relink) {
    requireInitialized();
    identifier(id);
    identifier(command);
    identifier(sourceId);
    revisionNumber(expected);
    const auto operation = nlohmann::json{
        {"action", relink ? "relink" : "attach_source"},
        {"source_id", sourceId},
        {"path", path}}.dump();
    // Reconcile receipt before touching a path that may since have moved or disappeared.
    Statement receipt(db_, "SELECT request_json,revision FROM workspace_commands WHERE "
                           "workspace_id=? AND command_id=?");
    receipt.bind(1, id);
    receipt.bind(2, command);
    if (receipt.step() == SQLITE_ROW) {
        const auto prior = nlohmann::json::parse(receipt.text(0));
        if (prior.at("operation") != operation || prior.at("expected") != expected)
            throw StoreError(StoreErrorCode::Invalid,
                             "Command identity reused with different content");
        const auto saved = read(id, receipt.text(1));
        if (!saved)
            throw StoreError(StoreErrorCode::Storage, "Source command receipt lost its revision");
        return *saved;
    }
    const auto saved = read(id, expected);
    if (!saved) throw StoreError(StoreErrorCode::NotFound, "Workspace revision not found");
    auto document = nlohmann::json::parse(saved->documentJson);
    if (!document.contains("sources")) document["sources"] = nlohmann::json::object();
    auto& sources = document.at("sources");
    if (!sources.is_object())
        throw StoreError(StoreErrorCode::Invalid, "Invalid source collection");
    if (relink != sources.contains(sourceId))
        throw StoreError(StoreErrorCode::Invalid,
                         relink ? "Source link not found" : "Source identity already exists");
    const auto checked = fingerprint(path);
    if (relink && (sources.at(sourceId).at("sha256") != checked.at("sha256") ||
                   sources.at(sourceId).at("size_bytes") != checked.at("size_bytes")))
        throw StoreError(StoreErrorCode::Conflict,
                         "Relink fingerprint mismatch; original link preserved");
    sources[sourceId] = checked;
    return write(id, expected, command, canonicalDocument(document.dump()), operation);
}
std::string WorkspaceStore::verifySource(const std::string& id, const std::string& revision,
                                         const std::string& sourceId) const {
    const auto saved = read(id, revision);
    if (!saved) throw StoreError(StoreErrorCode::NotFound, "Workspace revision not found");
    const auto document = nlohmann::json::parse(saved->documentJson);
    const auto& expected = document.at("sources").at(sourceId);
    const auto actual = fingerprint(expected.at("path").get<std::string>());
    const bool matches = actual.at("sha256") == expected.at("sha256") &&
                         actual.at("size_bytes") == expected.at("size_bytes");
    return nlohmann::json{
        {"status", matches ? "ok" : "conflict"},
        {"workspace_id", id},
        {"revision", revision},
        {"source_id", sourceId},
        {"verified", matches},
        {"message", matches ? "Source bytes match pinned fingerprint" : "Source bytes changed"}}
        .dump();
}
std::optional<Snapshot> WorkspaceStore::receipt(const std::string& id,
                                                const std::optional<std::string>& expected,
                                                const std::string& command,
                                                const std::string& document) const {
    requireInitialized();
    identifier(id);
    identifier(command);
    if (expected) revisionNumber(*expected);
    const auto request = nlohmann::json{
        {"expected", expected ? nlohmann::json(*expected) : nlohmann::json(nullptr)},
        {"document", canonicalDocument(document)},
        {"operation",
         "commit"}}.dump();
    Statement q(db_, "SELECT request_json,revision FROM workspace_commands WHERE workspace_id=? "
                     "AND command_id=?");
    q.bind(1, id);
    q.bind(2, command);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    if (q.text(0) != request)
        throw StoreError(StoreErrorCode::Invalid, "Command identity reused with different content");
    const auto saved = read(id, q.text(1));
    if (!saved) throw StoreError(StoreErrorCode::Storage, "Command references a missing revision");
    return saved;
}
std::optional<Snapshot> WorkspaceStore::derivedReceipt(const std::string& id,
                                                       const std::string& command,
                                                       const std::string& request) const {
    requireInitialized();
    identifier(id);
    identifier(command);
    const auto operation = "derive:" + nlohmann::json::parse(request).dump();
    Statement q(db_, "SELECT request_json,revision FROM workspace_commands WHERE workspace_id=? "
                     "AND command_id=?");
    q.bind(1, id);
    q.bind(2, command);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    if (nlohmann::json::parse(q.text(0)).at("operation") != operation)
        throw StoreError(StoreErrorCode::Invalid, "Derived command identity collision");
    const auto saved = read(id, q.text(1));
    if (!saved) throw StoreError(StoreErrorCode::Storage, "Derived receipt missing snapshot");
    return saved;
}
Snapshot WorkspaceStore::derive(const std::string& id, const std::string& command,
                                const std::string& request, const std::string& document) {
    Transaction tx(db_);
    if (auto previous = derivedReceipt(id, command, request)) {
        tx.commit();
        return *previous;
    }
    const auto origin = nlohmann::json::parse(request);
    const auto source = read(origin.at("source_workspace_id").get<std::string>());
    if (!source || source->revision != origin.at("source_revision").get<std::string>())
        throw StoreError(StoreErrorCode::Conflict, "Source workspace changed during planning");
    auto result = writeInTransaction(id, std::nullopt, command, canonicalDocument(document),
                                     "derive:" + origin.dump());
    tx.commit();
    return result;
}
Snapshot WorkspaceStore::commit(const std::string& id, const std::optional<std::string>& expected,
                                const std::string& command, const std::string& document) {
    return write(id, expected, command, canonicalDocument(document), "commit");
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
    Transaction tx(db_);
    auto result = writeInTransaction(id, expected, command, document, operation);
    tx.commit();
    return result;
}
Snapshot WorkspaceStore::writeInTransaction(const std::string& id,
                                            const std::optional<std::string>& expected,
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
    return Snapshot{id, std::to_string(next), document};
}
} // namespace backend::workspace
