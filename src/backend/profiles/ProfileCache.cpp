#include "backend/profiles/ProfileCache.h"
#include <sqlite3.h>
#include <limits>
#include <optional>

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
    r.releaseNotes = q.text(13); // appended column (after downloaded_at, synced_at)
    verifyRevision(r);
    return r;
}

bool hasColumn(sqlite3* db, const char* table, const char* column) {
    Statement q(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (q.step() == SQLITE_ROW)
        if (q.text(1) == column) return true;
    return false;
}

MethodDraft draftRow(const Statement& q) {
    MethodDraft d;
    d.draftId = q.text(0);
    d.projectId = q.text(1);
    d.methodId = q.text(2);
    d.newMethod = sqlite3_column_int(q.value, 3) == 1;
    d.methodDisplayName = q.text(4);
    d.methodDescription = q.text(5);
    d.baseRevisionId = q.text(6);
    d.configJson = q.text(7);
    d.cameraScript = q.text(8);
    d.processingCoreId = q.text(9);
    d.processingContractVersion = sqlite3_column_int(q.value, 10);
    d.hardwareCompatibilityJson = q.text(11);
    d.releaseNotes = q.text(12);
    d.revisionId = q.text(13);
    d.submittedRevisionId = q.text(14);
    d.createdAtUtc = q.text(15);
    d.updatedAtUtc = q.text(16);
    return d;
}

constexpr const char* kDraftColumns =
    "draft_id,project_id,method_id,new_method,method_name,method_description,base_revision_id,"
    "config_json,camera_script,core_id,contract_version,hardware_json,release_notes,revision_id,"
    "submitted_revision_id,created_at,updated_at";
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
    // #398 M3: release notes (older caches gain the column in place) and drafts.
    if (!hasColumn(impl_->db, "registry_revisions", "release_notes"))
        exec(impl_->db,
             "ALTER TABLE registry_revisions ADD COLUMN release_notes TEXT NOT NULL DEFAULT ''");
    exec(impl_->db, "CREATE TRIGGER IF NOT EXISTS registry_notes_immutable BEFORE UPDATE OF "
                    "release_notes ON registry_revisions WHEN OLD.release_notes<>'' "
                    "BEGIN SELECT RAISE(ABORT,'immutable release notes'); END;");
    exec(impl_->db, "CREATE TABLE IF NOT EXISTS registry_drafts (draft_id TEXT PRIMARY KEY,"
                    "project_id TEXT NOT NULL, method_id TEXT NOT NULL, new_method INTEGER NOT NULL,"
                    "method_name TEXT NOT NULL, method_description TEXT NOT NULL,"
                    "base_revision_id TEXT NOT NULL, config_json TEXT NOT NULL,"
                    "camera_script TEXT NOT NULL, core_id TEXT NOT NULL,"
                    "contract_version INTEGER NOT NULL, hardware_json TEXT NOT NULL,"
                    "release_notes TEXT NOT NULL, revision_id TEXT NOT NULL,"
                    "submitted_revision_id TEXT NOT NULL DEFAULT '',"
                    "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
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
    std::optional<Revision> old;
    {
        Statement existing(impl_->db, "SELECT * FROM registry_revisions WHERE revision_id=?");
        existing.bind(1, r.revisionId);
        if (existing.step() == SQLITE_ROW) old = row(existing);
    } // finalize the read before updateState() writes the same table
    if (old) {
        if (old->methodId != r.methodId || old->projectId != r.projectId ||
            old->parentRevisionId != r.parentRevisionId || old->authorId != r.authorId ||
            old->contentHash != r.contentHash || old->canonicalContent != r.canonicalContent ||
            old->revisionNumber != r.revisionNumber ||
            (!old->releaseNotes.empty() && old->releaseNotes != r.releaseNotes))
            throw RegistryError(RegistryErrorCode::Integrity,
                                "Attempt to replace immutable cached revision");
        updateState(r.revisionId, r.state, r.metadataVersion);
        if (old->releaseNotes.empty() && !r.releaseNotes.empty()) {
            // Cached before the server exposed notes; they are immutable there.
            Statement notes(impl_->db,
                            "UPDATE registry_revisions SET release_notes=? WHERE revision_id=? "
                            "AND release_notes=''");
            notes.bind(1, r.releaseNotes);
            notes.bind(2, r.revisionId);
            notes.step();
        }
    } else {
        Statement q(
            impl_->db,
            "INSERT INTO "
            "registry_revisions(revision_id,method_id,project_id,parent_id,display_name,author_id,"
            "content,hash,number,metadata_version,state,release_notes) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
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
        q.bind(12, r.releaseNotes);
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
std::vector<Revision> ProfileCache::listAll(std::vector<std::string>* corrupt) const {
    Statement q(impl_->db,
                "SELECT * FROM registry_revisions ORDER BY project_id,method_id,number DESC");
    std::vector<Revision> result;
    while (q.step() == SQLITE_ROW) {
        try {
            result.push_back(row(q));
        } catch (const RegistryError&) {
            if (corrupt) corrupt->push_back(q.text(0));
        }
    }
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
std::vector<LocalValidationRecord> ProfileCache::listValidations() const {
    Statement q(impl_->db,
                "SELECT revision_id,instrument_id,hash,context_hash,validator_id,evidence,passed,"
                "validated_at FROM registry_validations ORDER BY validated_at DESC,revision_id");
    std::vector<LocalValidationRecord> result;
    while (q.step() == SQLITE_ROW) {
        LocalValidationRecord record;
        record.validation.revisionId = q.text(0);
        record.validation.instrumentId = q.text(1);
        record.validation.contentHash = q.text(2);
        record.validation.contextHash = q.text(3);
        record.validation.validatorId = q.text(4);
        record.validation.evidence = q.text(5);
        record.validation.passed = sqlite3_column_int(q.value, 6) == 1;
        record.validatedAtUtc = q.text(7);
        result.push_back(std::move(record));
    }
    return result;
}
void ProfileCache::saveDraft(const MethodDraft& d) {
    if (d.draftId.empty() || d.projectId.empty() || d.methodId.empty() || d.revisionId.empty() ||
        d.methodDisplayName.empty())
        throw RegistryError(RegistryErrorCode::Invalid, "Draft identity incomplete");
    Transaction transaction(impl_->db);
    {
        Statement existing(impl_->db, "SELECT submitted_revision_id FROM registry_drafts WHERE draft_id=?");
        existing.bind(1, d.draftId);
        if (existing.step() == SQLITE_ROW && !existing.text(0).empty())
            throw RegistryError(RegistryErrorCode::Conflict, "A submitted draft cannot be changed");
    }
    Statement q(impl_->db,
                "INSERT INTO registry_drafts(draft_id,project_id,method_id,new_method,method_name,"
                "method_description,base_revision_id,config_json,camera_script,core_id,"
                "contract_version,hardware_json,release_notes,revision_id) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(draft_id) DO UPDATE SET "
                "project_id=excluded.project_id,method_id=excluded.method_id,"
                "new_method=excluded.new_method,method_name=excluded.method_name,"
                "method_description=excluded.method_description,"
                "base_revision_id=excluded.base_revision_id,config_json=excluded.config_json,"
                "camera_script=excluded.camera_script,core_id=excluded.core_id,"
                "contract_version=excluded.contract_version,hardware_json=excluded.hardware_json,"
                "release_notes=excluded.release_notes,revision_id=excluded.revision_id,"
                "updated_at=CURRENT_TIMESTAMP");
    q.bind(1, d.draftId);
    q.bind(2, d.projectId);
    q.bind(3, d.methodId);
    q.bind(4, uint64_t(d.newMethod));
    q.bind(5, d.methodDisplayName);
    q.bind(6, d.methodDescription);
    q.bind(7, d.baseRevisionId);
    q.bind(8, d.configJson);
    q.bind(9, d.cameraScript);
    q.bind(10, d.processingCoreId);
    q.bind(11, uint64_t(d.processingContractVersion < 0 ? 0 : d.processingContractVersion));
    q.bind(12, d.hardwareCompatibilityJson);
    q.bind(13, d.releaseNotes);
    q.bind(14, d.revisionId);
    q.step();
    transaction.commit();
}
MethodDraft ProfileCache::readDraft(const std::string& id) const {
    Statement q(impl_->db,
                (std::string("SELECT ") + kDraftColumns + " FROM registry_drafts WHERE draft_id=?").c_str());
    q.bind(1, id);
    if (q.step() != SQLITE_ROW) throw RegistryError(RegistryErrorCode::NotFound, "Draft not found");
    return draftRow(q);
}
std::vector<MethodDraft> ProfileCache::listDrafts() const {
    Statement q(impl_->db, (std::string("SELECT ") + kDraftColumns +
                            " FROM registry_drafts ORDER BY updated_at DESC,draft_id")
                               .c_str());
    std::vector<MethodDraft> result;
    while (q.step() == SQLITE_ROW) result.push_back(draftRow(q));
    return result;
}
void ProfileCache::deleteDraft(const std::string& id) {
    Statement q(impl_->db, "DELETE FROM registry_drafts WHERE draft_id=?");
    q.bind(1, id);
    q.step();
}
void ProfileCache::markDraftSubmitted(const std::string& id, const std::string& revisionId) {
    Statement q(impl_->db, "UPDATE registry_drafts SET submitted_revision_id=?,"
                           "updated_at=CURRENT_TIMESTAMP WHERE draft_id=?");
    q.bind(1, revisionId);
    q.bind(2, id);
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
