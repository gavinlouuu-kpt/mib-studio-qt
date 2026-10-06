// registry_facade_test (issue #398, bridge ABI 25)
//
// Frontend-neutral facade surface for the central profile registry:
//  - an uninitialized facade refuses every registry command and returns an
//    invalid snapshot;
//  - sign-in / refresh / download through the facade over a fake Supabase,
//    with contract-pinned integer enums (session, connectivity, job kind and
//    state, central state) and no credential in any value;
//  - facade shutdown aborts a hung registry request promptly;
//  - AppBackend loads the instrument identity (#398 M2) at initialize.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/profiles/ProfileRegistryWorker.h"

#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using backend::bridge::BackendFacade;
using backend::bridge::BackendRegistryJob;
using backend::bridge::BackendRegistrySnapshot;
using mib::test::FakeSupabase;
using mib::test::revisionJson;
using namespace std::chrono_literals;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

BackendRegistryJob waitJob(BackendFacade& facade, std::uint64_t id) {
    BackendRegistryJob job;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        MIB_REQUIRE(facade.fetchRegistryJob(id, job), "job known to the facade");
        if (job.state >= 2) return job; // Succeeded / Partial / Failed / Cancelled
        std::this_thread::sleep_for(5ms);
    }
    MIB_REQUIRE(false, "registry job did not finish");
    return job;
}

bool contains(const BackendRegistrySnapshot& s, const std::string& needle) {
    if (s.healthMessage.find(needle) != std::string::npos) return true;
    if (s.email.find(needle) != std::string::npos) return true;
    if (s.lastJob.message.find(needle) != std::string::npos) return true;
    for (const auto& r : s.revisions)
        if (r.contentHash.find(needle) != std::string::npos ||
            r.displayName.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir scratch("mib_registry_facade");
    const auto frames = scratch / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 4, 32, 32), "mock frames");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");
    setEnv("MIB_PROFILE_REGISTRY_URL", mib::test::kOrigin);
    setEnv("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY", mib::test::kKey);
    setEnv("MIB_INSTRUMENT_NAME", "MIB-test");

    FakeSupabase fake;
    fake.users["bob@lab"] = {"user-bob", "pw-bob", {"p1"}};
    fake.revisions["p1"] = {revisionJson("p1", "r1", 1, "published"),
                            revisionJson("p1", "r2", 2, "revoked")};

    watchdog.mark("uninitialized");
    {
        backend::AppBackend app;
        app.setProfileRegistryTransport(mib::test::transportFor(fake));
        BackendFacade facade(app);
        BackendRegistrySnapshot s;
        MIB_EXPECT(!facade.fetchRegistrySnapshot(s) && !s.valid, "no snapshot before initialize");
        MIB_EXPECT(facade.registrySignIn("bob@lab", "pw-bob") == 0 &&
                       facade.registryRefresh() == 0 && facade.registrySignOut() == 0 &&
                       facade.registryDownload("r1") == 0 && !facade.registryCancelAll(),
                   "uninitialized facade refuses registry commands");
    }

    watchdog.mark("sign in and refresh through the facade");
    {
        backend::AppBackend app;
        app.setProfileRegistryTransport(mib::test::transportFor(fake));
        BackendFacade facade(app);
        MIB_REQUIRE(facade.initialize((scratch / "data").string()), "facade initialize");
        MIB_EXPECT(backend::profiles::isInstrumentUuid(app.instrumentIdentity().id) &&
                       app.instrumentIdentity().name == "MIB-test",
                   "AppBackend loads the instrument identity at initialize");

        BackendRegistrySnapshot s;
        MIB_REQUIRE(facade.fetchRegistrySnapshot(s) && s.valid && s.configured,
                    "registry configured through env + transport");
        MIB_EXPECT(s.origin == mib::test::kOrigin, "origin reported");
        MIB_EXPECT(s.session == 0, "session 0 = SignedOut");

        const auto signIn = facade.registrySignIn("bob@lab", "pw-bob");
        const auto job = waitJob(facade, signIn);
        MIB_EXPECT(job.kind == 0 && job.state == 2, "kind 0 = SignIn, state 2 = Succeeded");
        MIB_REQUIRE(facade.fetchRegistrySnapshot(s), "snapshot");
        MIB_EXPECT(s.session == 1 && s.subjectId == "user-bob" && s.email == "bob@lab",
                   "session 1 = SignedIn");
        MIB_EXPECT(s.connectivity == 1, "connectivity 1 = Online");

        const auto refresh = waitJob(facade, facade.registryRefresh());
        MIB_EXPECT(refresh.kind == 2 && refresh.state == 2, "kind 2 = Refresh succeeded");
        MIB_REQUIRE(facade.fetchRegistrySnapshot(s), "snapshot");
        MIB_EXPECT(s.revisions.size() == 2 && s.projects.size() == 1 &&
                       s.projects[0].roles == std::vector<std::string>{"operator"},
                   "revisions and projects mirrored");
        bool published = false, revoked = false;
        for (const auto& r : s.revisions) {
            published |= r.revisionId == "r1" && r.centralState == 3;
            revoked |= r.revisionId == "r2" && r.centralState == 6;
        }
        MIB_EXPECT(published && revoked, "central state 3 = Published, 6 = Revoked");
        MIB_EXPECT(s.hasLastSuccessfulRefresh && s.lastSuccessfulRefreshUnixMs > 0,
                   "last refresh time as Unix ms");
        MIB_EXPECT(s.lastJob.jobId != 0 && s.lastJob.kind == 2, "last job mirrored");
        MIB_EXPECT(!contains(s, "pw-bob") && !contains(s, "access-") && !contains(s, "refresh-"),
                   "no password or token in the snapshot");

        const auto download = waitJob(facade, facade.registryDownload("r1"));
        MIB_EXPECT(download.kind == 3 && download.state == 2, "kind 3 = Download succeeded");
        BackendRegistryJob unknown;
        MIB_EXPECT(!facade.fetchRegistryJob(999999, unknown) && unknown.jobId == 0,
                   "unknown job is invalid");

        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = true;
        }
        const auto failed = waitJob(facade, facade.registryRefresh());
        MIB_EXPECT(failed.state == 4, "state 4 = Failed during an outage");
        MIB_REQUIRE(facade.fetchRegistrySnapshot(s), "snapshot");
        MIB_EXPECT(s.connectivity == 2 && s.revisions.size() == 2,
                   "connectivity 2 = Offline; cache still listed");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.offline = false;
        }

        watchdog.mark("shutdown aborts a hung request");
        fake.hang = true;
        const auto hung = facade.registryRefresh();
        MIB_REQUIRE(hung != 0, "refresh queued");
        while (fake.hungRequests.load() == 0)
            std::this_thread::sleep_for(1ms);
        const auto started = std::chrono::steady_clock::now();
        facade.shutdown();
        MIB_EXPECT(std::chrono::steady_clock::now() - started < 8s,
                   "facade shutdown does not wait out the registry timeout");
        MIB_EXPECT(app.profileRegistry().job(hung).state ==
                       backend::profiles::RegistryJobState::Cancelled,
                   "hung refresh cancelled by shutdown");
        MIB_EXPECT(facade.registryRefresh() == 0, "shut-down facade refuses commands");
        fake.hang = false;
    }
    return mib::test::exitCode();
}
