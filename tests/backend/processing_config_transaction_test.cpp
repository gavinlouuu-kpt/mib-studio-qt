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
    // Regression: the live JSON editor used to ignore enabled and mutate
    // processing before discovering an invalid later calibration field.
    backend.processing().setRealtimeEnabled(true);
    backend::bridge::ProcessingSettingsCommand live;
    live.configJson = R"({"realtime_processing":{"enabled":false}})";
    MIB_EXPECT(facade.dispatch(live).ok && !backend.processing().isRealtimeEnabled(),
               "live JSON honors realtime enabled");
    const auto before = backend.processing().getProcessingConfig().area_threshold_min;
    live.configJson =
        R"({"image_processing":{"area_threshold_min":12345},"pixel_to_micron":"invalid"})";
    bool threw = false;
    bool accepted = false;
    try {
        accepted = facade.dispatch(live).ok;
    } catch (...) {
        threw = true;
    }
    MIB_EXPECT(!threw && !accepted,
               "malformed full document returns rejection rather than throwing");
    MIB_EXPECT(backend.processing().getProcessingConfig().area_threshold_min == before,
               "late validation error leaves all processing settings unchanged");
    // PZ7035 profile settings (U-Net cells, Laplacian gate) apply through the
    // same path and are validated like the other ranges.
    live.configJson = R"({"image_processing":{"min_cell_area_px":300,"laplacian_kernel_size":1,)"
                      R"("laplacian_variance_min":10.5,"laplacian_variance_max":500,)"
                      R"("filters":{"enable_laplacian_variance_check":true}}})";
    MIB_EXPECT(facade.dispatch(live).ok, "cell and Laplacian settings apply");
    {
        const auto c = backend.processing().getProcessingConfig();
        MIB_EXPECT(c.min_cell_area_px == 300 && c.laplacian_kernel_size == 1 && c.enable_laplacian_variance_check &&
                       c.laplacian_variance_min == 10.5 && c.laplacian_variance_max == 500,
                   "cell and Laplacian settings reach the processing config");
    }
    for (const char* bad : {R"({"image_processing":{"laplacian_kernel_size":2}})",
                            R"({"image_processing":{"min_cell_area_px":70000}})",
                            R"({"image_processing":{"laplacian_variance_min":600}})"}) {
        live.configJson = bad;
        MIB_EXPECT(!facade.dispatch(live).ok, std::string("rejected: ") + bad);
    }
    MIB_EXPECT(backend.processing().getProcessingConfig().laplacian_kernel_size == 1,
               "a rejected setting leaves the config unchanged");
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
