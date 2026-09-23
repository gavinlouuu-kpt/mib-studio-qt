#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ProcessingCoreTrust.h"
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
int main() {
    mib::test::Watchdog watchdog;
    mib::test::TempDir temp;
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(temp.path().string()), "init");
    using J = nlohmann::json;
    const auto root = (temp / "cores").string();
    auto call = [&](J request) {
        return J::parse(facade.processingCoreCommand(root, request.dump()));
    };
    auto info = call({{"operation", "info"}});
    MIB_REQUIRE(info["ok"], info.dump());
    const auto original = info["active_version"];
    auto bundled = call({{"operation", "bundled"}});
    MIB_REQUIRE(bundled["ok"], bundled.dump());
    MIB_EXPECT(std::filesystem::exists(std::filesystem::path(root) / "selection.json"),
               "selection persisted");
    auto restored = call({{"operation", "restore"}});
    MIB_EXPECT(restored["ok"] && restored["active_version"] == original, "bundled roundtrip");
    auto mismatch =
        call({{"operation", "activate_local"},
              {"entry", {{"version", "2"}}},
              {"manifest_json", R"({"processing_core_manifest_schema_version":2,"version":"1"})"}});
    MIB_EXPECT(!mismatch["ok"] && call({{"operation", "info"}})["active_version"] == original,
               "mismatched immutable manifest cannot change core");
    std::ofstream(std::filesystem::path(root) / "selection.json") << "bad json";
    auto invalid = call({{"operation", "restore"}});
    MIB_EXPECT(!invalid["ok"] && !backend.processing().isProcessingCorePinSatisfied(),
               "corrupt persisted selection fails readiness closed");
    MIB_EXPECT(call({{"operation", "bundled"}})["ok"] &&
                   backend.processing().isProcessingCorePinSatisfied(),
               "explicit bundled recovery clears failed restoration");
    const auto fault = (temp / "occupied").string();
    std::ofstream(fault) << "keep";
    auto failed = J::parse(facade.processingCoreCommand(fault, R"({"operation":"bundled"})"));
    MIB_EXPECT(!failed["ok"] && call({{"operation", "info"}})["active_version"] == original,
               "persistence fault preserves active kernel");
    backend::app::ProcessingCoreSignaturePolicy policy;
    policy.required = true;
    policy.scheme = "unsupported";
    std::string error;
    auto verifier = backend::app::processingCoreTrustVerifier(policy);
    MIB_EXPECT(!verifier("missing", error) && !error.empty(),
               "shared production policy rejects unsupported signature schemes");
    std::vector<std::thread> workers;
    std::vector<J> results(8);
    for (size_t i = 0; i < 8; ++i)
        workers.emplace_back([&, i] { results[i] = call({{"operation", "bundled"}}); });
    for (auto& t : workers)
        t.join();
    for (const auto& r : results)
        MIB_EXPECT(r["ok"], r.dump());
    backend.shutdown();
    return mib::test::exitCode();
}
