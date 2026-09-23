#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
#include <vector>
int main() {
    mib::test::Watchdog watchdog;
    mib::test::TempDir temp;
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(temp.path().string()), "initialize backend");
    backend.setLastConfigJson(R"({"camera":{"identity":"actual-camera"},"roi":{"x":17}})");
    const auto path = (temp / "config.json").string();
    std::ofstream(path)
        << R"({"custom":{"keep":17},"image_processing":{"unknown":3,"area_threshold_max":333}})";
    auto baseline = facade.fetchConfigDocument(path);
    MIB_REQUIRE(baseline.ok, baseline.error);
    const std::string patch = R"({"image_processing":{"deformability_threshold_min":0.12345678}})";
    auto result = facade.applyConfigDocument(path, baseline.revision, patch);
    MIB_REQUIRE(result.saved && result.applied && result.verified, result.error);
    MIB_EXPECT(backend.processing().getProcessingConfig().area_threshold_max == 333,
               "unpatched disk values are authoritative over previous runtime values");
    const auto runtimeDocument = nlohmann::json::parse(backend.getLastConfigJson());
    MIB_EXPECT(runtimeDocument["camera"]["identity"] == "actual-camera" &&
                   runtimeDocument["roi"]["x"] == 17,
               "processing-only save preserves actual runtime non-processing provenance");
    MIB_EXPECT(!runtimeDocument.contains("custom"),
               "selected disk document does not become runtime provenance");
    auto saved = facade.fetchConfigDocument(path);
    auto json = nlohmann::json::parse(saved.documentJson);
    MIB_EXPECT(json["custom"]["keep"] == 17 && json["image_processing"]["unknown"] == 3,
               "unknown fields survive");
    MIB_EXPECT(json["image_processing"]["deformability_threshold_min"] == 0.12345678,
               "roundtrip precision");
    result = facade.applyConfigDocument(path, baseline.revision, patch);
    MIB_EXPECT(result.conflict && !result.saved && !result.applied, "stale baseline rejected");
    for (const auto* invalid : {R"({"image_processing":{"filters":null}})",
                                R"({"image_processing":{"area_threshold_min":1e30}})",
                                R"({"image_processing":{"unknown":3}})", R"({"roi":{}})"}) {
        result = facade.applyConfigDocument(path, saved.revision, invalid);
        MIB_EXPECT(!result.saved && !result.applied && !result.error.empty(),
                   "invalid patch unchanged");
        MIB_EXPECT(facade.fetchConfigDocument(path).revision == saved.revision,
                   "validation cannot write");
    }
    // Concurrent requests against one baseline: exactly one gets the save.
    std::vector<backend::app::ProcessingConfigTransactionResult> results(8);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < results.size(); ++i)
        threads.emplace_back([&, i] {
            results[i] = facade.applyConfigDocument(
                path, saved.revision,
                "{\"image_processing\":{\"area_threshold_min\":" + std::to_string(20 + i) + "}}");
        });
    for (auto& t : threads)
        t.join();
    int count = 0;
    for (const auto& r : results) {
        count += r.saved;
        MIB_EXPECT(r.verified || r.conflict, r.error);
    }
    MIB_EXPECT(count == 1, "baseline serialization prevents lost update");
    const auto oversized = (temp / "oversized.json").string();
    std::ofstream(oversized) << std::string(4 * 1024 * 1024 + 1, ' ');
    MIB_EXPECT(!facade.fetchConfigDocument(oversized).ok, "bounded document reads");
    baseline = facade.fetchConfigDocument(path);
    result = facade.applyConfigDocument(path, baseline.revision, std::string(65537, ' '));
    MIB_EXPECT(!result.saved && !result.error.empty(), "bounded patch input");
    // Filesystem fault: document replaced by directory after baseline read.
    baseline = facade.fetchConfigDocument(path);
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    result = facade.applyConfigDocument(path, baseline.revision, patch);
    MIB_EXPECT(!result.saved && !result.applied && !result.error.empty(),
               "filesystem fault does not apply");
    std::filesystem::remove(path);
    std::ofstream(path) << "invalid JSON";
    MIB_EXPECT(!facade.fetchConfigDocument(path).ok, "malformed document rejected");
    backend.shutdown();
    return mib::test::exitCode();
}
