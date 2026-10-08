// startup_configuration_test (#398 M2c): the applied configuration survives a
// restart through one startup pointer (<dataDir>/startup_configuration.json).
//  - a central Apply records the revision and its config sha256; after a
//    restart (a new backend on the same data dir, registry cache reopened
//    offline) the revision is re-applied exactly;
//  - a changed sha256 or a revoked revision is refused, nothing re-applied;
//  - last applied wins: a later profile Apply replaces the central pointer
//    and a later central Apply replaces the profile pointer;
//  - no pointer: nothing restored (or a legacy profile selection, if a
//    folder is given).
#include "backend/app/AppBackend.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/ProfileStore.h"
#include "backend/app/StartupConfiguration.h"
#include "backend/processing/ProcessingService.h"
#include "backend/profiles/ProfileRegistryWorker.h"
#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using backend::profiles::RegistryJobState;
using mib::test::FakeSupabase;
using mib::test::revisionJson;
using Json = nlohmann::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

RegistryJobState run(backend::profiles::ProfileRegistryWorker& worker, std::uint64_t id) {
    MIB_REQUIRE(id != 0, "registry request refused");
    MIB_REQUIRE(worker.waitForJob(id, 10s), "registry job finished");
    return worker.job(id).state;
}

Json restore(backend::AppBackend& backend, const std::string& base = {}) {
    return Json::parse(backend::app::restoreStartupConfiguration(backend, base));
}

} // namespace

int main() {
    mib::test::Watchdog wd(90);
    mib::test::TempDir td("startup_configuration");
    const auto data = td.path() / "data";
    const auto pointer = data / "startup_configuration.json";
    const auto profiles = (td.path() / "profiles").string();
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");
    setEnv("MIB_PROFILE_REGISTRY_URL", mib::test::kOrigin);
    setEnv("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY", mib::test::kKey);

    FakeSupabase fake;
    fake.users["bob@lab"] = {"user-bob", "pw-bob", {"p1"}};
    fake.revisions["p1"] = {revisionJson("p1", "r1", 1, "published"), revisionJson("p1", "r2", 2, "published")};

    std::string r1Config, r2Config;
    wd.mark("first session: apply r1");
    {
        backend::AppBackend backend;
        backend.setProfileRegistryTransport(mib::test::transportFor(fake));
        MIB_REQUIRE(backend.initialize(data.string()), "backend init");
        auto& registry = backend.profileRegistry();
        MIB_EXPECT(restore(backend)["restored"] == false, "no pointer, no folder: nothing restored");
        MIB_REQUIRE(run(registry, registry.requestSignIn("bob@lab", "pw-bob")) == RegistryJobState::Succeeded,
                    "sign in");
        MIB_REQUIRE(run(registry, registry.requestRefresh()) == RegistryJobState::Succeeded, "refresh");
        MIB_REQUIRE(run(registry, registry.requestMaterialize("r1")) == RegistryJobState::Succeeded, "r1");
        MIB_REQUIRE(run(registry, registry.requestMaterialize("r2")) == RegistryJobState::Succeeded, "r2");
        r1Config = readText(data / "methods" / "r1" / "config.json");
        r2Config = readText(data / "methods" / "r2" / "config.json");
        MIB_REQUIRE(!r1Config.empty() && !r2Config.empty(), "materialized");
        const auto applied = backend::app::applyCentralMethod(backend, "r1");
        MIB_REQUIRE(applied.ok, applied.error);
        const auto recorded = Json::parse(readText(pointer));
        MIB_EXPECT(recorded["kind"] == "central" && recorded["revision_id"] == "r1" &&
                       recorded["config_sha256"].get<std::string>().size() == 64,
                   "central apply records the revision and its sha256");
        backend.shutdown();
    }

    wd.mark("restart: r1 re-applied offline");
    {
        backend::AppBackend backend;
        backend.setProfileRegistryTransport(mib::test::transportFor(fake));
        MIB_REQUIRE(backend.initialize(data.string()), "backend init after restart");
        MIB_EXPECT(backend.getLastConfigJson() != r1Config, "nothing applied before restore");
        const auto r = restore(backend);
        MIB_REQUIRE(r["ok"] == true, r.dump());
        MIB_EXPECT(r["kind"] == "central" && r["restored"] == true && r["revision_id"] == "r1", "central restored");
        MIB_EXPECT(backend.getLastConfigJson() == r1Config, "r1's exact bytes re-applied after the restart");

        // A record whose sha256 no longer matches the cached revision is refused.
        auto tampered = Json::parse(readText(pointer));
        tampered["config_sha256"] = std::string(64, '0');
        std::ofstream(pointer, std::ios::binary | std::ios::trunc) << tampered.dump();
        backend.setLastConfigJson("{}");
        const auto changed = restore(backend);
        MIB_EXPECT(changed["ok"] == false && changed["error"].get<std::string>().find("changed") != std::string::npos &&
                       backend.getLastConfigJson() == "{}",
                   "changed method refused, nothing re-applied");

        wd.mark("last applied wins");
        // A profile applied after the method replaces the pointer...
        const auto created = Json::parse(backend::app::profileStoreCommand(
            backend, profiles,
            Json{{"operation", "create"}, {"name", "lab"}, {"document_json", R"({"pixel_to_micron_factor":0.42})"}}
                .dump()));
        MIB_REQUIRE(created["ok"] == true, created.dump());
        const auto applied = Json::parse(backend::app::profileStoreCommand(
            backend, profiles,
            Json{{"operation", "apply"}, {"name", "lab"}, {"baseline", created["profile"]["revision"]}}.dump()));
        MIB_REQUIRE(applied["ok"] == true, applied.dump());
        MIB_EXPECT(Json::parse(readText(pointer))["kind"] == "profile", "profile apply replaces the pointer");
        backend.processing().setPixelToMicronFactor(1.0);
        auto r2 = restore(backend);
        MIB_EXPECT(r2["ok"] == true && r2["kind"] == "profile" && r2["restored"] == true &&
                       backend.processing().getPixelToMicronFactor() == 0.42,
                   "the profile is restored from its recorded folder");
        // ...and a central method applied after the profile replaces it again.
        const auto central = backend::app::applyCentralMethod(backend, "r2");
        MIB_REQUIRE(central.ok, central.error);
        backend.processing().setPixelToMicronFactor(1.0);
        backend.setLastConfigJson("{}");
        r2 = restore(backend, profiles);
        MIB_EXPECT(r2["kind"] == "central" && r2["revision_id"] == "r2" && backend.getLastConfigJson() == r2Config &&
                       backend.processing().getPixelToMicronFactor() == 1.0,
                   "the later central method wins over the folder's profile selection");

        wd.mark("revoked startup method");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.revisions["p1"][1]["state"] = "revoked";
            fake.revisions["p1"][1]["metadata_version"] = 2;
        }
        auto& registry = backend.profileRegistry();
        MIB_REQUIRE(run(registry, registry.requestSignIn("bob@lab", "pw-bob")) == RegistryJobState::Succeeded,
                    "sign in again");
        MIB_REQUIRE(run(registry, registry.requestRefresh()) == RegistryJobState::Succeeded, "refresh");
        backend.setLastConfigJson("{}");
        const auto revoked = restore(backend);
        MIB_EXPECT(revoked["ok"] == false && backend.getLastConfigJson() == "{}",
                   "a revoked startup method is not re-applied");
        backend.shutdown();
    }

    wd.mark("legacy selection without a pointer");
    {
        fs::remove(pointer);
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize(data.string()), "backend init");
        MIB_EXPECT(restore(backend)["restored"] == false, "no pointer and no folder: nothing");
        const auto legacy = restore(backend, profiles);
        MIB_EXPECT(legacy["ok"] == true && legacy["kind"] == "profile" && legacy["restored"] == true &&
                       backend.processing().getPixelToMicronFactor() == 0.42,
                   "a legacy folder selection is still restored");
        backend.shutdown();
    }
    return mib::test::exitCode();
}
