// facade_reanalysis_pixel_factor_test (#758)
//
// MIB Studio's reanalysis (`review_reanalysis_json` on BackendFacade) ran on
// the live processing service and validated objects at its live factor, not
// the HDF source's recorded one. Fixture (review/pixel_factor_fixture.h): one
// bright disc per frame, valid and in the target group at 0.25 µm/px, not at
// 0.4886 or at the live 0.7 set here. Through the facade:
//  - an HDF source recorded at 0.25: validity and target group at 0.25, the
//    output records 0.25; the live factor stays 0.7, before, during (read
//    from the progress event) and after the job;
//  - a source recording no factor and a folder source: the live factor (0.7,
//    then the default 0.4886), recorded in the output;
//  - sources recorded at 0.25, 0.4886, 0.25 in turn: each its own factor.
// Watchdog guarded.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingService.h"

#include "review/pixel_factor_fixture.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace backend::bridge;
using namespace pixel_factor_fixture;
using nlohmann::json;

namespace
{
    json terminal(BackendFacade &facade, std::uint64_t id)
    {
        for (int i = 0; i < 6000; ++i)
        {
            auto status = json::parse(facade.fetchReviewReanalysisStatusJson());
            if (status.value("operation_id", "") == std::to_string(id) && status["state"] != "running" &&
                facade.activeOperationCount() == 0)
                return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        MIB_REQUIRE(false, "reanalysis terminal bounded wait");
        return {};
    }

    bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    mib::test::TempDir dir("facade_pixel_factor");
    const double areaPx = discAreaPx();
    const ProcessingConfig flip = flipConfig(areaPx);
    const auto at25 = dir / "recorded_025.h5";
    const auto at4886 = dir / "recorded_04886.h5";
    const auto none = dir / "records_none.h5";
    writeSource(at25, kRecorded, flip);
    writeSource(at4886, kDefault, flip);
    writeSource(none, 0.0, flip);
    const auto folder = dir / "folder";
    writeFolder(folder);

#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE", "mock");
#else
    setenv("MIB_CAMERA_MODE", "mock", 1);
#endif
    backend::AppBackend backend;
    BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(dir.path().string()), "initialize");
    backend.processing().setPixelToMicronFactor(0.7); // the live factor

    int n = 0;
    std::atomic<bool> liveFactorKept{true};
    facade.setEventSink([&](const BackendEvent &event) {
        if (const auto *op = std::get_if<OperationStatusEvent>(&event))
            if (op->kind == BackendOperationKind::Reanalysis && op->state == BackendOperationState::Progress &&
                !near(backend.processing().getPixelToMicronFactor(), 0.7) &&
                !near(backend.processing().getPixelToMicronFactor(), kDefault))
                liveFactorKept = false;
    });
    const auto reanalyse = [&](const std::filesystem::path &source, const std::string &kind) {
        const auto out = dir / ("out_" + std::to_string(n++) + ".h5");
        const json request{{"source_kind", kind},
                           {"source_path", source.string()},
                           {"output_path", out.string()},
                           {"dataset", "all"},
                           {"image_processing", backend::processing::config_json::toJson(flip)}};
        const auto started = facade.submitReviewReanalysisJson(request.dump());
        MIB_REQUIRE(started.ok, "reanalysis accepted: " + started.message);
        const auto status = terminal(facade, started.operationId);
        MIB_REQUIRE(status["state"] == "completed", "reanalysis completed: " + status.dump());
        return readOutput(out);
    };

    // A source recorded at 0.25 with the live factor 0.7.
    auto o = reanalyse(at25, "hdf");
    MIB_EXPECT(allObjects(o, true, true), "validity and target group at the recorded 0.25");
    MIB_EXPECT(o.recordedFactor && near(*o.recordedFactor, kRecorded), "the output records 0.25");
    MIB_EXPECT(near(backend.processing().getPixelToMicronFactor(), 0.7), "the live factor stays 0.7");
    wd.mark("recorded");

    // No recorded factor, and a folder: the live factor.
    o = reanalyse(none, "hdf");
    MIB_EXPECT(allObjects(o, false, false) && o.recordedFactor && near(*o.recordedFactor, 0.7),
               "a file without a factor: the live 0.7, recorded");
    o = reanalyse(folder, "folder");
    MIB_EXPECT(allObjects(o, false, false) && o.recordedFactor && near(*o.recordedFactor, 0.7),
               "a folder: the live 0.7, recorded");
    backend.processing().setPixelToMicronFactor(kDefault);
    o = reanalyse(none, "hdf");
    MIB_EXPECT(allObjects(o, false, false) && o.recordedFactor && near(*o.recordedFactor, kDefault),
               "with the live default: 0.4886, and the output says so");
    backend.processing().setPixelToMicronFactor(0.7);
    wd.mark("fallback");

    // Sources with different factors in turn: each its own.
    o = reanalyse(at25, "hdf");
    MIB_EXPECT(allObjects(o, true, true) && o.recordedFactor && near(*o.recordedFactor, kRecorded), "first 0.25");
    o = reanalyse(at4886, "hdf");
    MIB_EXPECT(allObjects(o, false, false) && o.recordedFactor && near(*o.recordedFactor, kDefault), "then 0.4886");
    o = reanalyse(at25, "hdf");
    MIB_EXPECT(allObjects(o, true, true) && o.recordedFactor && near(*o.recordedFactor, kRecorded), "then 0.25 again");
    MIB_EXPECT(liveFactorKept.load(), "the live factor never changed while a job ran");
    MIB_EXPECT(near(backend.processing().getPixelToMicronFactor(), 0.7), "the live factor is still 0.7");
    wd.mark("sequence");

    facade.setEventSink({});
    facade.shutdown();
    return mib::test::exitCode();
}
