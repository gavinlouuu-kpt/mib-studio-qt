// The profile cache and registry stamp their rows with the synced wall clock (#651 G14 follow-up), not SQLite's CURRENT_TIMESTAMP: the PZ7035 has
// no RTC, so the board's own time reads whatever the boot left. Without a sync the system clock is used, as before.
#include "backend/app/WallClock.h"
#include "backend/profiles/ProfileCache.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <sqlite3.h>

#include <string>

using namespace backend::profiles;
using backend::app::WallClock;

namespace {
Revision revision(const std::string& id, uint64_t number) {
    Revision r;
    r.methodId = "method";
    r.revisionId = id;
    r.projectId = "project";
    r.authorId = "alice";
    r.displayName = "name";
    r.parentRevisionId = "";
    r.revisionNumber = number;
    r.metadataVersion = 1;
    r.state = CentralState::Approved;
    r.canonicalContent = canonicalMethod(R"({"config_schema_version":1,"gain":2})", "", "core", 1);
    r.contentHash = contentHash(r.canonicalContent);
    return r;
}

std::string cell(const std::string& path, const std::string& sql) {
    sqlite3* db = nullptr;
    std::string out = "<error>";
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}
} // namespace

int main() {
    mib::test::TempDir temp;
    const std::string path = (temp.path() / "cache.sqlite3").string();
    WallClock::resetForTesting();
    WallClock::setBoardWithoutRtc(true);
    // The board's own clock says 2025-05-29; a client then tells it it is 2026-10-10 12:00:00 UTC.
    constexpr int64_t kBoardNs = 1748544872LL * 1'000'000'000LL;
    WallClock::setClocksForTesting(5'000'000'000LL, kBoardNs);

    ProfileCache cache(path, "https://registry.example", "bob");
    const Revision r = revision("r1", 1);
    cache.store(r);
    MIB_EXPECT(cell(path, "SELECT downloaded_at FROM registry_revisions WHERE revision_id='r1'").rfind("2025-05-29", 0) == 0,
               "unsynced: the stamps come from the system clock as before: " + cell(path, "SELECT downloaded_at FROM registry_revisions"));

    MIB_REQUIRE(WallClock::sync(1791633600000LL, false), "a client syncs the clock");  // 2026-10-10 12:00:00 UTC
    WallClock::setClocksForTesting(5'000'000'000LL + 3'000'000'000LL, kBoardNs);  // 3 s later by the monotonic clock
    cache.store(revision("r2", 2));
    MIB_EXPECT(cell(path, "SELECT downloaded_at FROM registry_revisions WHERE revision_id='r2'") == "2026-10-10 12:00:03",
               "synced: a revision is stamped with the client-synced time: " + cell(path, "SELECT downloaded_at FROM registry_revisions WHERE revision_id='r2'"));
    MIB_EXPECT(cell(path, "SELECT synced_at FROM registry_revisions WHERE revision_id='r2'") == "2026-10-10 12:00:03", "synced_at too");

    cache.updateState("r2", CentralState::Superseded, 2);
    MIB_EXPECT(cell(path, "SELECT synced_at FROM registry_revisions WHERE revision_id='r2'") == "2026-10-10 12:00:03", "updateState stamps synced_at");
    WallClock::setClocksForTesting(5'000'000'000LL + 63'000'000'000LL, kBoardNs);
    cache.updateState("r2", CentralState::Revoked, 3);
    MIB_EXPECT(cell(path, "SELECT synced_at FROM registry_revisions WHERE revision_id='r2'") == "2026-10-10 12:01:03", "a later update moves synced_at with the wall clock");
    MIB_EXPECT(cell(path, "SELECT downloaded_at FROM registry_revisions WHERE revision_id='r2'") == "2026-10-10 12:00:03", "downloaded_at stays");

    LocalValidation v{"r2", "MIB-01", r.contentHash, "ctx", "bob", "evidence", true};
    LocalValidation own = v;
    own.contentHash = cache.read("r2").contentHash;
    cache.recordValidation(own);
    const auto listed = cache.listValidations();
    MIB_REQUIRE(listed.size() == 1, "one validation");
    MIB_EXPECT(listed[0].validatedAtUtc == "2026-10-10 12:01:03", "a validation carries the wall-clock time: " + listed[0].validatedAtUtc);
    WallClock::setClocksForTesting(5'000'000'000LL + 123'000'000'000LL, kBoardNs);
    cache.recordValidation(own);
    MIB_EXPECT(cache.listValidations()[0].validatedAtUtc == "2026-10-10 12:02:03", "re-validating restamps it");

    MethodDraft d;
    d.draftId = "d1"; d.projectId = "project"; d.methodId = "method"; d.revisionId = "r3"; d.methodDisplayName = "name";
    d.configJson = R"({"config_schema_version":1})";
    d.processingCoreId = "core";
    cache.saveDraft(d);
    MIB_EXPECT(cell(path, "SELECT created_at FROM registry_drafts WHERE draft_id='d1'") == "2026-10-10 12:02:03" &&
                   cell(path, "SELECT updated_at FROM registry_drafts WHERE draft_id='d1'") == "2026-10-10 12:02:03",
               "a draft is stamped created and updated with the wall clock");
    WallClock::setClocksForTesting(5'000'000'000LL + 183'000'000'000LL, kBoardNs);
    cache.saveDraft(d);
    MIB_EXPECT(cell(path, "SELECT created_at FROM registry_drafts WHERE draft_id='d1'") == "2026-10-10 12:02:03" &&
                   cell(path, "SELECT updated_at FROM registry_drafts WHERE draft_id='d1'") == "2026-10-10 12:03:03",
               "saving again moves updated_at only");
    cache.markDraftSubmitted("d1", "r3");
    MIB_EXPECT(cell(path, "SELECT updated_at FROM registry_drafts WHERE draft_id='d1'") == "2026-10-10 12:03:03", "markDraftSubmitted stamps updated_at");

    WallClock::resetForTesting();
    return mib::test::exitCode();
}
