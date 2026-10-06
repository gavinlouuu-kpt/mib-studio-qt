// registry_authoring_test (#398 M3): the issue's authoring lifecycle through
// two real ProfileRegistryWorkers (author Alice, reviewer/publisher Carol) over
// a fake Supabase:
//  - local drafts (new method, or copied from a cached revision) are saved and
//    listed offline; an uncanonicalizable draft is refused; nothing is sent
//    until an explicit submit;
//  - submit creates the method when needed, carries immutable release notes,
//    is idempotent (pre-generated revision ID; a submitted draft is read-only);
//  - the author cannot approve her own revision; Carol approves and publishes;
//    publishing supersedes the previous head in Carol's cache at once; the
//    history lists the review and audit events;
//  - a draft whose base is no longer the head stops with a SubmitConflict
//    that compares upstream and draft changes, sends nothing, and can be
//    submitted explicitly as a branch (whose publication the server refuses
//    until resolved) or discarded;
//  - "update available" names the newer published revision;
//  - a viewer cannot submit; transitions need a reason;
//  - an older cache without the release_notes column is migrated in place.
#include "backend/app/MethodApply.h"
#include "backend/profiles/InstrumentIdentity.h"
#include "backend/profiles/ProfileRegistryWorker.h"

#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>

using namespace backend::profiles;
using mib::test::FakeSupabase;
using mib::test::transportFor;
using namespace std::chrono_literals;

namespace {

RegistryWorkerConfig config(const std::filesystem::path& dir) {
    RegistryWorkerConfig c;
    c.origin = mib::test::kOrigin;
    c.publishableKey = mib::test::kKey;
    c.cacheDir = dir;
    return c;
}

RegistryJobStatus run(ProfileRegistryWorker& worker, std::uint64_t id) {
    MIB_REQUIRE(id != 0, "request refused");
    MIB_REQUIRE(worker.waitForJob(id, 10s), "job did not finish");
    return worker.job(id);
}

const CachedRevisionSummary* byNumber(const RegistryWorkerSnapshot& s, uint64_t number) {
    for (const auto& r : s.revisions)
        if (r.revisionNumber == number) return &r;
    return nullptr;
}

const MethodDraft* draftById(const RegistryWorkerSnapshot& s, const std::string& id) {
    for (const auto& d : s.drafts)
        if (d.draftId == id) return &d;
    return nullptr;
}

MethodDraft newMethodDraft(const std::string& id, int gain) {
    MethodDraft d;
    d.draftId = id;
    d.projectId = "p1";
    d.newMethod = true;
    d.methodDisplayName = "Cell Sorting";
    d.methodDescription = "Sorting by deformability";
    d.configJson = R"({"config_schema_version":1,"gain":)" + std::to_string(gain) + "}";
    d.cameraScript = "camera();";
    d.processingCoreId = "core";
    d.processingContractVersion = 1;
    d.releaseNotes = "First release";
    return d;
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(90);
    mib::test::TempDir dir("mib_registry_authoring");
    FakeSupabase fake;
    fake.users["alice@lab"] = {"user-alice", "pw-alice", {"p1"}, {"author"}};
    fake.users["carol@lab"] = {"user-carol", "pw-carol", {"p1"}, {"reviewer", "publisher"}};
    fake.users["bob@lab"] = {"user-bob", "pw-bob", {"p1"}, {"viewer"}};

    ProfileRegistryWorker alice(config(dir / "alice"), transportFor(fake));
    ProfileRegistryWorker carol(config(dir / "carol"), transportFor(fake));
    MIB_REQUIRE(alice.waitIdle(5s) && carol.waitIdle(5s), "workers idle");

    watchdog.mark("refusals before sign-in");
    MIB_EXPECT(run(alice, alice.requestSaveDraft(newMethodDraft("d1", 1))).state == RegistryJobState::Failed,
               "no cache open: drafts need a signed-in (or cached) user");
    MIB_EXPECT(alice.requestTransition("r", CentralState::Approved, "  ") == 0, "blank reason refused");
    MIB_EXPECT(alice.requestTransition("r", CentralState::Submitted, "x") == 0, "Submitted is not a transition");
    MIB_EXPECT(alice.requestSubmitDraft("") == 0 && alice.requestDeleteDraft("") == 0, "empty IDs refused");

    MIB_REQUIRE(run(alice, alice.requestSignIn("alice@lab", "pw-alice")).state == RegistryJobState::Succeeded,
                "alice signs in");
    MIB_REQUIRE(run(carol, carol.requestSignIn("carol@lab", "pw-carol")).state == RegistryJobState::Succeeded,
                "carol signs in");
    MIB_REQUIRE(run(alice, alice.requestRefresh()).state == RegistryJobState::Succeeded, "alice refresh");
    MIB_EXPECT(alice.snapshot().methods.empty(), "no methods yet");

    watchdog.mark("local drafts");
    {
        auto bad = newMethodDraft("bad", 1);
        bad.configJson = R"({"gain":1})"; // no config_schema_version
        const auto refused = run(alice, alice.requestSaveDraft(bad));
        MIB_EXPECT(refused.state == RegistryJobState::Failed && refused.message.find("not saved") != std::string::npos,
                   "uncanonicalizable draft refused");
        const int before = fake.requests.load();
        MIB_REQUIRE(run(alice, alice.requestSaveDraft(newMethodDraft("d1", 1))).state == RegistryJobState::Succeeded,
                    "draft saved");
        MIB_EXPECT(fake.requests.load() == before, "saving a draft sends nothing");
        const auto s = alice.snapshot();
        const auto* d = draftById(s, "d1");
        MIB_REQUIRE(d != nullptr, "draft listed");
        MIB_EXPECT(isInstrumentUuid(d->methodId) && isInstrumentUuid(d->revisionId),
                   "method and revision IDs pre-generated");
        MIB_EXPECT(d->submittedRevisionId.empty() && d->baseRevisionId.empty(), "unsubmitted new-method draft");
    }

    watchdog.mark("submit");
    std::string r1;
    {
        const auto job = run(alice, alice.requestSubmitDraft("d1"));
        MIB_REQUIRE(job.state == RegistryJobState::Succeeded, job.message);
        MIB_EXPECT(job.kind == RegistryJobKind::SubmitDraft && job.message.find("r1") != std::string::npos, "r1 submitted");
        const auto s = alice.snapshot();
        const auto* rev = byNumber(s, 1);
        MIB_REQUIRE(rev != nullptr, "submitted revision cached");
        r1 = rev->revisionId;
        MIB_EXPECT(rev->state == CentralState::Submitted && rev->displayName == "Cell Sorting", "submitted state");
        MIB_EXPECT(draftById(s, "d1")->submittedRevisionId == r1, "draft marked submitted");
        MIB_EXPECT(s.methods.size() == 1 && s.methods[0].headRevisionId.empty(), "method created, no head yet");
        const int submits = fake.submits.load();
        MIB_EXPECT(run(alice, alice.requestSubmitDraft("d1")).message.find("Already submitted") != std::string::npos &&
                       fake.submits.load() == submits,
                   "resubmitting a submitted draft sends nothing");
        auto edited = newMethodDraft("d1", 5);
        MIB_EXPECT(run(alice, alice.requestSaveDraft(edited)).state == RegistryJobState::Failed,
                   "a submitted draft is read-only");
    }

    watchdog.mark("review and publish");
    {
        const auto own = run(alice, alice.requestTransition(r1, CentralState::Approved, "self review"));
        MIB_EXPECT(own.state == RegistryJobState::Failed, "the author cannot approve her own revision");
        MIB_REQUIRE(run(carol, carol.requestRefresh()).state == RegistryJobState::Succeeded, "carol refresh");
        MIB_REQUIRE(run(carol, carol.requestTransition(r1, CentralState::Approved, "checked on MIB-01")).state ==
                        RegistryJobState::Succeeded,
                    "carol approves");
        const auto published = run(carol, carol.requestTransition(r1, CentralState::Published, "pilot"));
        MIB_REQUIRE(published.state == RegistryJobState::Succeeded, published.message);
        auto s = carol.snapshot();
        MIB_EXPECT(byNumber(s, 1)->state == CentralState::Published, "published in carol's cache");
        MIB_EXPECT(s.methods.size() == 1 && s.methods[0].headRevisionId == r1, "head is r1");
        MIB_REQUIRE(run(carol, carol.requestHistory(r1)).state == RegistryJobState::Succeeded, "history");
        s = carol.snapshot();
        MIB_REQUIRE(s.history.has_value() && s.history->revisionId == r1, "history published");
        MIB_EXPECT(s.history->reviews.size() == 1 && s.history->reviews[0].decision == "approved" &&
                       s.history->reviews[0].reason == "checked on MIB-01",
                   "review recorded");
        std::vector<std::string> actions;
        for (const auto& e : s.history->events) actions.push_back(e.action);
        MIB_EXPECT((actions == std::vector<std::string>{"submitted", "approved", "published"}), "audit trail");
        const auto stale = run(carol, carol.requestTransition(r1, CentralState::Published, "again"));
        MIB_EXPECT(stale.state == RegistryJobState::Failed, "invalid/stale transition refused");
    }

    watchdog.mark("drafts from a published revision");
    MIB_REQUIRE(run(alice, alice.requestRefresh()).state == RegistryJobState::Succeeded, "alice refresh");
    const auto copyDraft = [&](const std::string& id, int gain) {
        MethodDraft d;
        d.draftId = id;
        MIB_REQUIRE(run(alice, alice.requestSaveDraft(d, r1)).state == RegistryJobState::Succeeded, "copied");
        auto copied = *draftById(alice.snapshot(), id);
        MIB_EXPECT(copied.baseRevisionId == r1 && !copied.newMethod && copied.cameraScript == "camera();" &&
                       copied.configJson.find("\"gain\": 1") != std::string::npos,
                   "content, method and base copied from r1");
        copied.configJson = R"({"config_schema_version":1,"gain":)" + std::to_string(gain) + "}";
        copied.releaseNotes = "gain " + std::to_string(gain);
        MIB_REQUIRE(run(alice, alice.requestSaveDraft(copied)).state == RegistryJobState::Succeeded, "edited");
    };
    copyDraft("dA", 2);
    copyDraft("dB", 3);

    std::string r2;
    {
        MIB_REQUIRE(run(alice, alice.requestSubmitDraft("dA")).state == RegistryJobState::Succeeded, "dA submitted");
        r2 = byNumber(alice.snapshot(), 2)->revisionId;
        MIB_EXPECT(byNumber(alice.snapshot(), 2)->releaseNotes == "gain 2", "release notes cached");
        MIB_REQUIRE(run(carol, carol.requestRefresh()).state == RegistryJobState::Succeeded, "carol refresh");
        MIB_REQUIRE(run(carol, carol.requestTransition(r2, CentralState::Approved, "ok")).state ==
                        RegistryJobState::Succeeded,
                    "r2 approved");
        MIB_REQUIRE(run(carol, carol.requestTransition(r2, CentralState::Published, "replaces r1")).state ==
                        RegistryJobState::Succeeded,
                    "r2 published");
        MIB_EXPECT(byNumber(carol.snapshot(), 1)->state == CentralState::Superseded,
                   "publishing supersedes r1 in carol's cache at once");
    }

    watchdog.mark("conflict: base is no longer the head");
    {
        const int submits = fake.submits.load();
        const auto job = run(alice, alice.requestSubmitDraft("dB"));
        MIB_EXPECT(job.state == RegistryJobState::Failed && job.message.find("Conflict") != std::string::npos,
                   "stale base stops the submit");
        MIB_EXPECT(fake.submits.load() == submits, "nothing was sent");
        const auto s = alice.snapshot();
        MIB_REQUIRE(s.submitConflict.has_value(), "conflict published");
        MIB_EXPECT(s.submitConflict->draftId == "dB" && s.submitConflict->baseRevisionId == r1 &&
                       s.submitConflict->headRevisionId == r2,
                   "base r1, head r2");
        MIB_EXPECT(s.submitConflict->compared &&
                       s.submitConflict->upstreamChanges == std::vector<std::string>{"gain"} &&
                       s.submitConflict->draftVsHead == std::vector<std::string>{"gain"},
                   "upstream and draft changes compared");

        const auto branch = run(alice, alice.requestSubmitDraft("dB", true));
        MIB_REQUIRE(branch.state == RegistryJobState::Succeeded, branch.message);
        MIB_EXPECT(!alice.snapshot().submitConflict.has_value(), "conflict cleared by the explicit branch");
        const auto afterBranch = alice.snapshot();
        const auto* r3 = byNumber(afterBranch, 3);
        MIB_REQUIRE(r3 != nullptr, "branch revision cached");
        MIB_EXPECT(r3->parentRevisionId == r1, "branch parent is the draft's base");
        MIB_REQUIRE(run(carol, carol.requestRefresh()).state == RegistryJobState::Succeeded, "carol refresh");
        MIB_REQUIRE(run(carol, carol.requestTransition(r3->revisionId, CentralState::Approved, "ok")).state ==
                        RegistryJobState::Succeeded,
                    "branch approved");
        const auto refused = run(carol, carol.requestTransition(r3->revisionId, CentralState::Published, "x"));
        MIB_EXPECT(refused.state == RegistryJobState::Failed, "publishing a stale branch is refused");
    }

    watchdog.mark("discard + update available");
    {
        MethodDraft d;
        d.draftId = "dC";
        MIB_REQUIRE(run(alice, alice.requestSaveDraft(d, r1)).state == RegistryJobState::Succeeded, "dC");
        MIB_REQUIRE(run(alice, alice.requestDeleteDraft("dC")).state == RegistryJobState::Succeeded, "discarded");
        MIB_EXPECT(draftById(alice.snapshot(), "dC") == nullptr, "draft gone");
        MIB_REQUIRE(run(alice, alice.requestRefresh()).state == RegistryJobState::Succeeded, "alice refresh");
        const auto s = alice.snapshot();
        MIB_EXPECT(backend::app::newerPublishedRevision(s, *byNumber(s, 1)) == r2, "r1: r2 available");
        MIB_EXPECT(backend::app::newerPublishedRevision(s, *byNumber(s, 2)).empty(), "r2 is the newest");
    }

    watchdog.mark("offline drafts; viewer cannot submit");
    {
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = true;
        }
        MIB_EXPECT(run(alice, alice.requestSaveDraft(newMethodDraft("dOff", 9))).state == RegistryJobState::Succeeded,
                   "drafts save offline");
        MIB_EXPECT(run(alice, alice.requestSubmitDraft("dOff")).state == RegistryJobState::Failed &&
                       alice.snapshot().health.connectivity == RegistryHealth::Connectivity::Offline,
                   "submit offline fails as an outage, draft kept");
        MIB_EXPECT(draftById(alice.snapshot(), "dOff") != nullptr, "draft kept");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = false;
        }
        ProfileRegistryWorker bob(config(dir / "bob"), transportFor(fake));
        MIB_REQUIRE(bob.waitIdle(5s), "bob idle");
        MIB_REQUIRE(run(bob, bob.requestSignIn("bob@lab", "pw-bob")).state == RegistryJobState::Succeeded, "bob");
        MIB_REQUIRE(run(bob, bob.requestSaveDraft(newMethodDraft("dBob", 4))).state == RegistryJobState::Succeeded,
                    "a viewer may keep local drafts");
        const auto job = run(bob, bob.requestSubmitDraft("dBob"));
        MIB_EXPECT(job.state == RegistryJobState::Failed &&
                       bob.snapshot().health.connectivity == RegistryHealth::Connectivity::PermissionDenied,
                   "a viewer cannot submit");
    }

    watchdog.mark("old cache gains release_notes in place");
    {
        const auto path = (dir / "old.sqlite").string();
        sqlite3* db = nullptr;
        MIB_REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open");
        MIB_REQUIRE(sqlite3_exec(db,
                                 "CREATE TABLE registry_scope (id INTEGER PRIMARY KEY CHECK(id=1), origin TEXT "
                                 "NOT NULL, subject TEXT NOT NULL);"
                                 "INSERT INTO registry_scope VALUES(1,'https://registry.example','user-x');"
                                 "CREATE TABLE registry_revisions (revision_id TEXT PRIMARY KEY, method_id TEXT NOT "
                                 "NULL, project_id TEXT NOT NULL, parent_id TEXT NOT NULL, display_name TEXT NOT "
                                 "NULL, author_id TEXT NOT NULL, content TEXT NOT NULL, hash TEXT NOT NULL, number "
                                 "INTEGER NOT NULL CHECK(number>0), metadata_version INTEGER NOT NULL "
                                 "CHECK(metadata_version>0), state TEXT NOT NULL, downloaded_at TEXT NOT NULL "
                                 "DEFAULT CURRENT_TIMESTAMP, synced_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);",
                                 nullptr, nullptr, nullptr) == SQLITE_OK,
                    "old schema");
        sqlite3_close(db);
        ProfileCache cache(path, "https://registry.example", "user-x");
        Revision r;
        r.methodId = "m";
        r.revisionId = "rv";
        r.projectId = "p";
        r.displayName = "M";
        r.authorId = "a";
        r.canonicalContent = canonicalMethod(R"({"config_schema_version":1})", "", "core", 1);
        r.contentHash = contentHash(r.canonicalContent);
        r.revisionNumber = 1;
        r.metadataVersion = 1;
        r.state = CentralState::Published;
        cache.store(r); // cached before the server had notes
        r.releaseNotes = "notes";
        cache.store(r); // backfilled once
        MIB_EXPECT(cache.read("rv").releaseNotes == "notes", "release notes backfilled");
        r.releaseNotes = "rewritten";
        bool threw = false;
        try {
            cache.store(r);
        } catch (const RegistryError&) {
            threw = true;
        }
        MIB_EXPECT(threw && cache.read("rv").releaseNotes == "notes", "release notes immutable once known");
    }
    return mib::test::exitCode();
}
