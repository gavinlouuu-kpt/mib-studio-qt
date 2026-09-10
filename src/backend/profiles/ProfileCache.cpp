#include "backend/profiles/ProfileCache.h"
#include <sqlite3.h>
#include <limits>

namespace backend::profiles {
namespace {
void check(int code) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw RegistryError(RegistryErrorCode::Storage, "Profile cache database operation failed");
}
struct Statement {
    sqlite3_stmt* value{nullptr};
    Statement(sqlite3* db, const char* sql) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr));
    }
    ~Statement() { sqlite3_finalize(value); }
    void bind(int index, const std::string& value) {
        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw RegistryError(RegistryErrorCode::Invalid, "Cache field too large");
        check(sqlite3_bind_text(this->value, index, value.data(), static_cast<int>(value.size()),
                                SQLITE_TRANSIENT));
    }
    void bind(int index, uint64_t value) {
        if (value > INT64_MAX)
            throw RegistryError(RegistryErrorCode::Invalid, "Revision counter overflow");
        check(sqlite3_bind_int64(this->value, index, static_cast<sqlite3_int64>(value)));
    }
    int step() {
        int result = sqlite3_step(value);
        check(result);
        return result;
    }
    std::string text(int index) const {
        const auto* data = sqlite3_column_text(value, index);
        return data ? std::string(reinterpret_cast<const char*>(data),
                                  sqlite3_column_bytes(value, index))
                    : "";
    }
};
void exec(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
struct Transaction {
    sqlite3* db;
    bool committed{false};
    explicit Transaction(sqlite3* db) : db(db) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        exec(db, "COMMIT");
        committed = true;
    }
};
Revision row(const Statement& q) {
    Revision r;
    r.revisionId = q.text(0);
    r.methodId = q.text(1);
    r.projectId = q.text(2);
    r.parentRevisionId = q.text(3);
    r.displayName = q.text(4);
    r.authorId = q.text(5);
    r.canonicalContent = q.text(6);
    r.contentHash = q.text(7);
    r.revisionNumber = sqlite3_column_int64(q.value, 8);
    r.metadataVersion = sqlite3_column_int64(q.value, 9);
    r.state = centralStateFromString(q.text(10));
    verifyRevision(r);
    return r;
}
} // namespace

struct ProfileCache::Impl {
    sqlite3* db{nullptr};
    ~Impl() {
        if (db) sqlite3_close(db);
    }
};

ProfileCache::ProfileCache(const std::string& path, const std::string& registryOrigin,
                           const std::string& subjectId)
    : impl_(std::make_unique<Impl>()) {
    if (registryOrigin.empty() || subjectId.empty())
        throw RegistryError(RegistryErrorCode::Invalid,
                            "Cache requires registry and user identity");
    check(sqlite3_open_v2(path.c_str(), &impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          nullptr));
    check(sqlite3_busy_timeout(impl_->db, 1000));
    exec(impl_->db, "PRAGMA foreign_keys=ON");
    Transaction transaction(impl_->db);
    exec(impl_->db, "CREATE TABLE IF NOT EXISTS registry_scope (id INTEGER PRIMARY KEY "
                    "CHECK(id=1), origin TEXT NOT NULL, subject TEXT NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS registry_revisions ("
                    "revision_id TEXT PRIMARY KEY, method_id TEXT NOT NULL, project_id TEXT NOT "
                    "NULL, parent_id TEXT NOT NULL,"
                    "display_name TEXT NOT NULL, author_id TEXT NOT NULL, content TEXT NOT NULL, "
                    "hash TEXT NOT NULL,"
                    "number INTEGER NOT NULL CHECK(number>0), metadata_version INTEGER NOT NULL "
                    "CHECK(metadata_version>0), state TEXT NOT NULL,"
                    "downloaded_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, synced_at TEXT NOT "
                    "NULL DEFAULT CURRENT_TIMESTAMP);"
                    "CREATE TABLE IF NOT EXISTS registry_validations (revision_id TEXT NOT NULL "
                    "REFERENCES registry_revisions(revision_id),"
                    "instrument_id TEXT NOT NULL, hash TEXT NOT NULL, context_hash TEXT NOT NULL, "
                    "validator_id TEXT NOT NULL, evidence TEXT NOT NULL,"
                    "passed INTEGER NOT NULL, validated_at TEXT NOT NULL DEFAULT "
                    "CURRENT_TIMESTAMP, PRIMARY KEY(revision_id,instrument_id,context_hash));"
                    "CREATE TRIGGER IF NOT EXISTS registry_immutable BEFORE UPDATE OF "
                    "revision_id,method_id,project_id,parent_id,author_id,content,hash,number"
                    " ON registry_revisions BEGIN SELECT RAISE(ABORT,'immutable revision'); END;"
                    "CREATE TRIGGER IF NOT EXISTS registry_metadata_guard BEFORE UPDATE OF "
                    "state,metadata_version ON registry_revisions "
                    "WHEN (OLD.state='revoked' AND NEW.state<>'revoked') OR "
                    "NEW.metadata_version<OLD.metadata_version "
                    "OR (NEW.metadata_version=OLD.metadata_version AND NEW.state<>OLD.state) "
                    "BEGIN SELECT RAISE(ABORT,'invalid metadata transition'); END;");
    Statement scope(impl_->db, "SELECT origin,subject FROM registry_scope WHERE id=1");
    if (scope.step() == SQLITE_ROW) {
        if (scope.text(0) != registryOrigin || scope.text(1) != subjectId)
            throw RegistryError(RegistryErrorCode::Permission,
                                "Cache belongs to a different registry or user");
    } else {
        Statement insert(impl_->db, "INSERT INTO registry_scope VALUES(1,?,?)");
        insert.bind(1, registryOrigin);
        insert.bind(2, subjectId);
        insert.step();
    }
    transaction.commit();
}
ProfileCache::~ProfileCache() = default;

void ProfileCache::store(const Revision& r) {
    verifyRevision(r);
    Transaction transaction(impl_->db);
    Statement existing(impl_->db, "SELECT * FROM registry_revisions WHERE revision_id=?");
    existing.bind(1, r.revisionId);
    if (existing.step() == SQLITE_ROW) {
        const auto old = row(existing);
        if (old.methodId != r.methodId || old.projectId != r.projectId ||
            old.parentRevisionId != r.parentRevisionId || old.authorId != r.authorId ||
            old.contentHash != r.contentHash || old.canonicalContent != r.canonicalContent ||
            old.revisionNumber != r.revisionNumber)
            throw RegistryError(RegistryErrorCode::Integrity,
                                "Attempt to replace immutable cached revision");
        updateState(r.revisionId, r.state, r.metadataVersion);
    } else {
        Statement q(
            impl_->db,
            "INSERT INTO "
            "registry_revisions(revision_id,method_id,project_id,parent_id,display_name,author_id,"
            "content,hash,number,metadata_version,state) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        q.bind(1, r.revisionId);
        q.bind(2, r.methodId);
        q.bind(3, r.projectId);
        q.bind(4, r.parentRevisionId);
        q.bind(5, r.displayName);
        q.bind(6, r.authorId);
        q.bind(7, r.canonicalContent);
        q.bind(8, r.contentHash);
        q.bind(9, r.revisionNumber);
        q.bind(10, r.metadataVersion);
        q.bind(11, toString(r.state));
        q.step();
    }
    transaction.commit();
}

Revision ProfileCache::read(const std::string& id) const {
    Statement q(impl_->db, "SELECT * FROM registry_revisions WHERE revision_id=?");
    q.bind(1, id);
    if (q.step() != SQLITE_ROW)
        throw RegistryError(RegistryErrorCode::NotFound, "Revision not cached");
    return row(q);
}
std::vector<Revision> ProfileCache::list(const std::string& project) const {
    Statement q(
        impl_->db,
        "SELECT * FROM registry_revisions WHERE project_id=? ORDER BY method_id,number DESC");
    q.bind(1, project);
    std::vector<Revision> result;
    while (q.step() == SQLITE_ROW)
        result.push_back(row(q));
    return result;
}
void ProfileCache::updateState(const std::string& id, CentralState state, uint64_t version) {
    const auto old = read(id);
    if (version < old.metadataVersion)
        return; // out-of-order response; never roll metadata backward
    if (version == old.metadataVersion && state != old.state)
        throw RegistryError(RegistryErrorCode::Conflict, "Conflicting revision metadata version");
    if (old.state == CentralState::Revoked && state != CentralState::Revoked)
        throw RegistryError(RegistryErrorCode::Conflict, "Revocation is terminal");
    Statement q(
        impl_->db,
        "UPDATE registry_revisions SET state=?,metadata_version=?,synced_at=CURRENT_TIMESTAMP "
        "WHERE revision_id=? AND metadata_version<=?");
    q.bind(1, toString(state));
    q.bind(2, version);
    q.bind(3, id);
    q.bind(4, version);
    q.step();
}
void ProfileCache::recordValidation(const LocalValidation& v) {
    const auto r = read(v.revisionId);
    if (v.contentHash != r.contentHash || v.instrumentId.empty() || v.contextHash.empty() ||
        v.validatorId.empty() || v.evidence.empty())
        throw RegistryError(RegistryErrorCode::Invalid,
                            "Validation requires exact content/context identity and evidence");
    Statement q(impl_->db,
                "INSERT INTO "
                "registry_validations(revision_id,instrument_id,hash,context_hash,validator_id,"
                "evidence,passed) VALUES(?,?,?,?,?,?,?) ON "
                "CONFLICT(revision_id,instrument_id,context_hash) DO UPDATE SET "
                "hash=excluded.hash,validator_id=excluded.validator_id,evidence=excluded.evidence,"
                "passed=excluded.passed,validated_at=CURRENT_TIMESTAMP");
    q.bind(1, v.revisionId);
    q.bind(2, v.instrumentId);
    q.bind(3, v.contentHash);
    q.bind(4, v.contextHash);
    q.bind(5, v.validatorId);
    q.bind(6, v.evidence);
    q.bind(7, uint64_t(v.passed));
    q.step();
}
Eligibility ProfileCache::eligibility(const std::string& id, const std::string& instrument,
                                      const std::string& context) const {
    try {
        const auto r = read(id);
        if (r.state != CentralState::Published && r.state != CentralState::Superseded)
            return {false, std::string("Central state: ") + toString(r.state)};
        Statement q(impl_->db, "SELECT passed FROM registry_validations WHERE revision_id=? AND "
                               "instrument_id=? AND hash=? AND context_hash=?");
        q.bind(1, id);
        q.bind(2, instrument);
        q.bind(3, r.contentHash);
        q.bind(4, context);
        if (q.step() != SQLITE_ROW)
            return {false, "Local validation required for this instrument/context"};
        if (sqlite3_column_int(q.value, 0) != 1) return {false, "Local validation failed"};
        return {true, "Cached integrity and local validation passed; Apply and hardware readiness "
                      "still required"};
    } catch (const RegistryError& e) {
        return {false, e.what()};
    }
}
} // namespace backend::profiles
