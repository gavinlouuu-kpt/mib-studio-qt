// ProfileRegistryWorker (#398 M1): sign-in, refresh-token rotation, paged
// refresh across member projects, offline continuity across restart, explicit
// sign-out, cancellation of a hung request, bounded refresh, and concurrent
// snapshot/command traffic during shutdown. A fake Supabase (Auth + RPC) sits
// behind the injected transport; no network, no hardware.
#include "backend/profiles/ProfileRegistryWorker.h"

#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace backend::profiles;
using Json = nlohmann::json;
using namespace std::chrono_literals;
using mib::test::FakeSupabase;
using mib::test::kKey;
using mib::test::kOrigin;
using mib::test::revisionJson;
using mib::test::transportFor;

namespace {

RegistryWorkerConfig config(const std::filesystem::path& dir) {
    RegistryWorkerConfig c;
    c.origin = kOrigin;
    c.publishableKey = kKey;
    c.cacheDir = dir;
    return c;
}

RegistryJobStatus run(ProfileRegistryWorker& worker, std::uint64_t id) {
    MIB_REQUIRE(id != 0, "request refused");
    MIB_REQUIRE(worker.waitForJob(id, 10s), "job did not finish");
    return worker.job(id);
}

bool hasRevision(const RegistryWorkerSnapshot& s, const std::string& id) {
    for (const auto& r : s.revisions)
        if (r.revisionId == id) return true;
    return false;
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir dir("mib_registry_worker");
    FakeSupabase fake;
    fake.users["alice@lab"] = {"user-alice", "pw-alice", {"p1", "p2"}};
    fake.users["bob@lab"] = {"user-bob", "pw-bob", {"p2"}};
    fake.revisions["p1"] = {revisionJson("p1", "r1", 1), revisionJson("p1", "r2", 2)};
    fake.revisions["p2"] = {revisionJson("p2", "r3", 1)};

    watchdog.mark("unconfigured");
    {
        ProfileRegistryWorker inert({}, transportFor(fake));
        MIB_EXPECT(!inert.snapshot().configured, "no origin: unconfigured");
        MIB_EXPECT(inert.requestRefresh() == 0, "unconfigured worker refuses requests");
        auto bad = config(dir.path());
        bad.publishableKey = "service_role_secret";
        ProfileRegistryWorker rejected(bad, transportFor(fake));
        MIB_EXPECT(!rejected.snapshot().configured && rejected.snapshot().health.connectivity ==
                                                          RegistryHealth::Connectivity::Failed,
                   "a non-publishable key disables the worker");
    }

    watchdog.mark("sign-in rejected");
    {
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_EXPECT(worker.snapshot().session == RegistryWorkerSnapshot::Session::SignedOut,
                   "fresh cache dir: signed out");
        MIB_EXPECT(worker.requestSignIn("alice@lab", "") == 0, "empty password refused");
        auto status = run(worker, worker.requestSignIn("alice@lab", "wrong"));
        MIB_EXPECT(status.state == RegistryJobState::Failed, "bad password fails");
        auto s = worker.snapshot();
        MIB_EXPECT(s.health.connectivity == RegistryHealth::Connectivity::AuthenticationRequired,
                   "bad password reported as authentication");
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::SignedOut, "still signed out");
        MIB_EXPECT(run(worker, worker.requestRefresh()).state == RegistryJobState::Failed,
                   "refresh needs sign-in");

        watchdog.mark("sign-in + refresh");
        status = run(worker, worker.requestSignIn("alice@lab", "pw-alice"));
        MIB_REQUIRE(status.state == RegistryJobState::Succeeded, status.message);
        s = worker.snapshot();
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::SignedIn &&
                       s.subjectId == "user-alice" && s.email == "alice@lab",
                   "signed in as alice");
        MIB_EXPECT(s.revisions.empty(), "sign-in does not sync by itself");
        status = run(worker, worker.requestRefresh());
        MIB_REQUIRE(status.state == RegistryJobState::Succeeded, status.message);
        s = worker.snapshot();
        MIB_EXPECT(s.projects.size() == 2, "both member projects listed");
        MIB_EXPECT(s.revisions.size() == 3 && hasRevision(s, "r1") && hasRevision(s, "r2") &&
                       hasRevision(s, "r3"),
                   "every member project's revisions cached");
        MIB_EXPECT(s.lastSuccessfulRefresh.has_value(), "refresh time recorded");
        MIB_EXPECT(s.health.connectivity == RegistryHealth::Connectivity::Online, "online");

        watchdog.mark("access token revoked early");
        fake.revisions["p1"].push_back(revisionJson("p1", "r4", 3));
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.rejectNextAccess = true;
        }
        status = run(worker, worker.requestRefresh());
        MIB_EXPECT(status.state == RegistryJobState::Succeeded,
                   "401 rotates the refresh token once and retries: " + status.message);
        MIB_EXPECT(hasRevision(worker.snapshot(), "r4"), "retry completed the refresh");

        watchdog.mark("outage keeps cache");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = true;
        }
        status = run(worker, worker.requestRefresh());
        MIB_EXPECT(status.state == RegistryJobState::Failed, "outage fails the refresh");
        s = worker.snapshot();
        MIB_EXPECT(s.health.connectivity == RegistryHealth::Connectivity::Offline,
                   "outage reported as registry offline");
        MIB_EXPECT(s.revisions.size() == 4, "outage leaves every cached revision listed");
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::SignedIn,
                   "an outage is not a sign-out");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = false;
        }
        MIB_EXPECT(run(worker, worker.requestDownload("r3")).state == RegistryJobState::Succeeded,
                   "reconnect: explicit download works");
        MIB_EXPECT(run(worker, worker.requestDownload("missing")).state == RegistryJobState::Failed,
                   "unknown revision fails truthfully");
    }

    watchdog.mark("restart offline");
    {
        // Restart with the registry unreachable: alice's cache reopens from
        // last_session.json without any token; nothing token-bearing persisted.
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = true;
        }
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        auto s = worker.snapshot();
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::CachedOffline &&
                       s.subjectId == "user-alice",
                   "last user's cache reopened offline");
        MIB_EXPECT(s.revisions.size() == 4, "cached revisions listable offline");
        MIB_EXPECT(run(worker, worker.requestRefresh()).state == RegistryJobState::Failed,
                   "network commands need a new sign-in");
        MIB_EXPECT(worker.snapshot().revisions.size() == 4, "failed refresh changed nothing");
        for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
            if (entry.path().extension() != ".json") continue;
            std::ifstream in(entry.path());
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            MIB_EXPECT(text.find("access-") == std::string::npos &&
                           text.find("refresh-") == std::string::npos &&
                           text.find("pw-alice") == std::string::npos,
                       "no token or password persisted in " + entry.path().string());
        }
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = false;
        }

        watchdog.mark("switch user");
        auto status = run(worker, worker.requestSignIn("bob@lab", "pw-bob"));
        MIB_REQUIRE(status.state == RegistryJobState::Succeeded, status.message);
        s = worker.snapshot();
        MIB_EXPECT(s.subjectId == "user-bob" && s.revisions.empty(),
                   "bob gets his own cache; alice's revisions are not shown to bob");
        MIB_EXPECT(run(worker, worker.requestRefresh()).state == RegistryJobState::Succeeded,
                   "bob refresh");
        s = worker.snapshot();
        MIB_EXPECT(s.revisions.size() == 1 && hasRevision(s, "r3"), "bob sees only p2");

        watchdog.mark("sign out");
        status = run(worker, worker.requestSignOut());
        MIB_EXPECT(status.state == RegistryJobState::Succeeded, "sign-out");
        s = worker.snapshot();
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::SignedOut && s.revisions.empty() &&
                       s.subjectId.empty(),
                   "sign-out closes the user's cache");
        MIB_EXPECT(!std::filesystem::exists(dir.path() / "last_session.json"),
                   "sign-out forgets the last session");
    }

    watchdog.mark("signed-out restart");
    {
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_EXPECT(worker.snapshot().session == RegistryWorkerSnapshot::Session::SignedOut,
                   "after explicit sign-out no cache reopens");
        // Alice's cache file survived: signing back in shows her methods at once.
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "alice back");
        MIB_EXPECT(worker.snapshot().revisions.size() == 4, "alice's cache kept on disk");

        watchdog.mark("expired refresh token");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.refreshTokens.clear(); // server forgot/rotated every refresh token
            fake.rejectNextAccess = true;
        }
        auto status = run(worker, worker.requestRefresh());
        MIB_EXPECT(status.state == RegistryJobState::Failed, "unrecoverable session fails");
        auto s = worker.snapshot();
        MIB_EXPECT(s.session == RegistryWorkerSnapshot::Session::CachedOffline &&
                       s.health.connectivity ==
                           RegistryHealth::Connectivity::AuthenticationRequired,
                   "expired session drops tokens but keeps the cache readable");
        MIB_EXPECT(s.revisions.size() == 4, "cache intact after session expiry");

        watchdog.mark("proactive token refresh");
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "alice re-signs in");
    }

    watchdog.mark("short-lived token");
    {
        // Tokens that expire inside the refresh margin are rotated before use.
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.expiresIn = 30;
        }
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "sign-in");
        std::size_t before = 0;
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            before = fake.paths.size();
        }
        MIB_EXPECT(run(worker, worker.requestRefresh()).state == RegistryJobState::Succeeded,
                   "refresh with short-lived tokens");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            MIB_EXPECT(before < fake.paths.size() &&
                           fake.paths[before] == "/auth/v1/token?grant_type=refresh_token",
                       "near-expiry token rotated before the first RPC");
            fake.expiresIn = 3600;
        }
    }

    watchdog.mark("bounded refresh");
    {
        auto c = config(dir.path());
        c.maxPagesPerRefresh = 2;
        ProfileRegistryWorker worker(c, transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "sign-in");
        const auto status = run(worker, worker.requestRefresh());
        MIB_EXPECT(status.state == RegistryJobState::Partial, "page budget ends as Partial");
        MIB_EXPECT(worker.snapshot().revisions.size() == 4, "partial refresh keeps the cache");
    }

    watchdog.mark("cancel hung request");
    {
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(worker.waitIdle(5s), "initial load");
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "sign-in");
        const auto healthBefore = worker.snapshot().health.connectivity;
        fake.hang = true;
        const auto hung = worker.requestRefresh();
        const auto queued = worker.requestDownload("r1");
        MIB_EXPECT(worker.requestRefresh() == hung ||
                       worker.job(hung).state == RegistryJobState::Running,
                   "a queued refresh is coalesced");
        while (fake.hungRequests.load() == 0)
            std::this_thread::sleep_for(1ms);
        const auto snap = worker.snapshot(); // must not wait for the hung request
        MIB_EXPECT(snap.busy, "busy while a request is in flight");
        worker.cancelAll();
        MIB_REQUIRE(worker.waitIdle(5s), "cancel unblocks the worker");
        MIB_EXPECT(worker.job(hung).state == RegistryJobState::Cancelled, "running job cancelled");
        MIB_EXPECT(worker.job(queued).state == RegistryJobState::Cancelled, "queued job dropped");
        MIB_EXPECT(worker.snapshot().health.connectivity == healthBefore,
                   "an aborted request is not reported as an outage");
        fake.hang = false;
        MIB_EXPECT(run(worker, worker.requestRefresh()).state == RegistryJobState::Succeeded,
                   "worker usable after cancel");

        watchdog.mark("cancel hung sign-in");
        fake.hang = true;
        const auto hungSignIn = worker.requestSignIn("alice@lab", "pw-alice");
        while (fake.hungRequests.load() < 2)
            std::this_thread::sleep_for(1ms);
        worker.cancelAll();
        MIB_REQUIRE(worker.waitIdle(5s), "cancel unblocks a hung sign-in");
        MIB_EXPECT(worker.job(hungSignIn).state == RegistryJobState::Cancelled,
                   "sign-in cancelled");
        const auto afterSignIn = worker.snapshot();
        MIB_EXPECT(afterSignIn.health.connectivity == RegistryHealth::Connectivity::Online,
                   "an aborted sign-in is not reported as an outage");
        MIB_EXPECT(afterSignIn.session == RegistryWorkerSnapshot::Session::SignedIn,
                   "a cancelled re-sign-in keeps the existing session");
        fake.hang = false;

        watchdog.mark("shutdown during hung request");
        fake.hang = true;
        worker.requestRefresh();
        while (fake.hungRequests.load() < 3)
            std::this_thread::sleep_for(1ms);
        worker.shutdown();
        worker.shutdown();
        MIB_EXPECT(worker.requestRefresh() == 0, "shut-down worker refuses requests");
        fake.hang = false;
    }

    watchdog.mark("concurrent snapshot/command traffic");
    {
        ProfileRegistryWorker worker(config(dir.path()), transportFor(fake));
        MIB_REQUIRE(run(worker, worker.requestSignIn("alice@lab", "pw-alice")).state ==
                        RegistryJobState::Succeeded,
                    "sign-in");
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> maxGeneration{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; !stop; ++i) {
                    const auto s = worker.snapshot();
                    std::uint64_t seen = maxGeneration.load();
                    while (s.generation > seen &&
                           !maxGeneration.compare_exchange_weak(seen, s.generation)) {
                    }
                    if (i % 7 == 0) {
                        if (t == 0) worker.requestRefresh();
                        if (t == 1) worker.requestDownload("r2");
                        if (t == 2 && i % 49 == 0) worker.cancelAll();
                    }
                }
            });
        }
        std::this_thread::sleep_for(300ms);
        watchdog.mark("shutdown under traffic");
        worker.shutdown();
        stop = true;
        for (auto& th : threads)
            th.join();
        MIB_EXPECT(maxGeneration.load() > 0, "snapshots observed progress");
    }

    return mib::test::exitCode();
}
