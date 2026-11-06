// Live tuning of the cell acceptance and target group criteria during an Active experiment
// (epic #866 MON-080; the Qt Monitoring tune panel applies them mid-run, the Tauri backend used to
// refuse every processing change while a run was active).
//
// Drives a real run through the facade with a mock camera and checks, on the runtime path
// (ProcessingSettingsCommand.configJson) and on the persistent document path
// (applyConfigDocument):
//  - while Active a patch of tune criteria only applies, reads back, and changes the provenance
//    text's image_processing and nothing else;
//  - any other key (non-tune image_processing keys, multi_image, ROI, realtime, typed commands) is
//    refused with the existing "locked during an experiment" reason, and nothing changes;
//  - the document path writes exactly the patch to the file and applies exactly the patch to the
//    running configuration, never the document's other (stale) values;
//  - Starting and Stopping refuse (observed from the status events), the table of refusals holds
//    for every state, and a run that has been asked to stop refuses;
//  - a thread that keeps tuning while Stop finalizes never succeeds after the run left Active, and
//    the last successful tune is what the runtime and the provenance end up with.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/app/ProcessingConfigTransaction.h"
#include "backend/app/SciencePlacement.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace bridge = backend::bridge;
namespace app = backend::app;
using Json = nlohmann::json;

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bridge::BackendCommandResult tune(bridge::BackendFacade& facade, const std::string& json) {
    bridge::ProcessingSettingsCommand command;
    command.configJson = json;
    return facade.dispatch(command);
}

std::string imageProcessing(const Json& patch) { return Json{{"image_processing", patch}}.dump(); }
} // namespace

int main() {
    mib::test::Watchdog watchdog(120);
    mib::test::TempDir temp("tune_apply");
    const auto dataDir = temp.path();
    const auto mockDir = dataDir / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 512, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (dataDir / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
    MIB_REQUIRE(app::hostProcessingAvailable(), "this test needs the host processing build");

    // The refusal table, without driving a run into every state.
    {
        using State = app::ExperimentRunState;
        std::string why;
        for (const auto state : {State::Idle, State::Starting, State::Stopping, State::Failed}) {
            MIB_EXPECT(!app::ExperimentCoordinator::midRunTuneAllowed(state, true, false, true, false, &why),
                       "only an Active run is tuned mid-run");
        }
        MIB_EXPECT(!app::ExperimentCoordinator::midRunTuneAllowed(State::Active, false, false, true, false, &why),
                   "an Active state without a run is refused");
        MIB_EXPECT(!app::ExperimentCoordinator::midRunTuneAllowed(State::Active, true, true, true, false, &why) &&
                       why.find("stopped") != std::string::npos,
                   "an Active run that is being stopped is refused, saying so");
        MIB_EXPECT(!app::ExperimentCoordinator::midRunTuneAllowed(State::Active, true, false, false, false, &why) &&
                       why.find("PL") != std::string::npos,
                   "a PL instrument is refused, saying so");
        MIB_EXPECT(!app::ExperimentCoordinator::midRunTuneAllowed(State::Active, true, false, true, true, &why) &&
                       why.find("calibration") != std::string::npos,
                   "a calibration method's run is refused, saying so (#879 review)");
        MIB_EXPECT(app::ExperimentCoordinator::midRunTuneAllowed(State::Active, true, false, true, false, &why) &&
                       why.empty(),
                   "an Active host-processed run that is not stopping is tuned");
    }

    // The whitelist.
    {
        std::string why;
        MIB_EXPECT(app::processingPatchIsTuneOnly(
                       Json::parse(R"({"area_threshold_min":10,"filters":{"enable_border_check":false},)"
                                   R"("target_group":{"emodulus_enabled":true,"emodulus_max":9.5}})"),
                       &why),
                   "tune keys are accepted: " + why);
        for (const char* other : {R"({"gaussian_blur_size":5})", R"({"multi_image":{"count":2}})",
                                  R"({"filters":{"enable_laplacian_variance_check":true}})",
                                  R"({"auto_background_enabled":true})", R"({"unknown":1})",
                                  R"({"area_threshold_min":10,"morph_iterations":2})",
                                  R"({"target_group":{"nope":1}})", R"({"filters":5})", R"({})"}) {
            MIB_EXPECT(!app::processingPatchIsTuneOnly(Json::parse(other), &why) && !why.empty(),
                       std::string("refused: ") + other);
        }
        MIB_EXPECT(!app::processingPatchIsTuneOnly(Json::parse("[1]"), &why), "not an object");
        MIB_EXPECT(!app::processingPatchIsTuneOnly(Json::parse(R"({"filters":{"enable_border_check":{"a":1}}})"), &why) &&
                       why == "image_processing.filters.enable_border_check",
                   "the refused path is dotted: " + why);
    }

    backend::AppBackend backend;
    bridge::BackendFacade facade(backend);

    std::mutex eventsMutex;
    std::atomic<bool> stoppingSeen{false};
    std::atomic<int> startingProbes{0}, stoppingProbes{0};
    std::atomic<bool> transitionAccepted{false};
    const std::string transitionPatch = imageProcessing(Json{{"area_threshold_min", 33}});
    facade.setEventSink([&](const bridge::BackendEvent& event) {
        if (const auto* status = std::get_if<bridge::ExperimentStatusEvent>(&event)) {
            const auto state = status->status.state;
            if (state == app::ExperimentRunState::Starting || state == app::ExperimentRunState::Stopping) {
                if (state == app::ExperimentRunState::Stopping) stoppingSeen.store(true);
                // The sink runs outside the coordinator's lock; a tune from here must be refused.
                const auto result = tune(facade, transitionPatch);
                if (result.ok || result.message.find("locked during an experiment") == std::string::npos)
                    transitionAccepted.store(true);
                (state == app::ExperimentRunState::Starting ? startingProbes : stoppingProbes).fetch_add(1);
            }
        }
    });
    MIB_REQUIRE(facade.initialize(dataDir.string()), "facade initializes");

    bridge::CameraCommand configure;
    configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
    configure.mockFrameDirectory = mockDir.string();
    configure.mockFrameIntervalMs = 1;
    configure.mockLoopFiles = true;
    MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configures");
    bridge::CameraCommand startCapture;
    startCapture.action = bridge::CameraCommandAction::StartCapture;
    MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture starts");

    // Known starting point, including settings the run must keep.
    auto& processing = backend.processing();
    {
        auto cfg = processing.getProcessingConfig();
        cfg.area_threshold_min = 60;
        cfg.area_threshold_max = 290;
        cfg.enable_area_range_check = true;
        cfg.enable_border_check = true;
        cfg.enable_target_group = false;
        cfg.gaussian_blur_size = 3;
        cfg.multi_image_enabled = false;
        cfg.multi_image_count = 1;
        processing.setProcessingConfig(cfg);
    }
    backend.setLastConfigJson(R"({"marker":{"keep":7},"roi":{"x":1,"y":2,"w":3,"h":4}})");

    const std::string runPath = (dataDir / "tune_run.h5").string();
    MIB_REQUIRE(waitFor([&] {
                    app::ExperimentReadinessSnapshot readiness;
                    return facade.fetchExperimentReadiness(readiness, runPath) && readiness.ready;
                }, 10000),
                "readiness passes with a running mock camera");

    // Idle: unchanged behaviour (a non-tune key applies, as before).
    {
        const auto idle = tune(facade, imageProcessing(Json{{"gaussian_blur_size", 5}, {"morph_iterations", 2}}));
        MIB_EXPECT(idle.ok && processing.getProcessingConfig().gaussian_blur_size == 5,
                   "idle still applies any processing key: " + idle.message);
        MIB_EXPECT(tune(facade, imageProcessing(Json{{"gaussian_blur_size", 3}, {"morph_iterations", 1}})).ok, "and takes it back");
    }

    app::ExperimentReadinessSnapshot readiness;
    MIB_REQUIRE(facade.fetchExperimentReadiness(readiness, runPath), "readiness");
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = runPath;
    start.readinessGeneration = readiness.generation;
    const auto started = facade.dispatch(start);
    MIB_REQUIRE(started.ok, "experiment starts: " + started.message);
    app::ExperimentStatus status;
    MIB_REQUIRE(waitFor([&] { return facade.fetchExperimentStatus(status) && status.state == app::ExperimentRunState::Active; }, 5000),
                "experiment is Active");
    MIB_EXPECT(startingProbes.load() >= 1, "the Starting status was probed");

    // ---- Active: the runtime path --------------------------------------------------------------
    // The run's provenance text, as the finalization will write it: a tune changes its image_processing only.
    backend.setLastConfigJson(R"({"marker":{"keep":7},"roi":{"x":1,"y":2,"w":3,"h":4},"pixel_to_micron_factor":9.5,)"
                              R"("realtime_processing":{"enabled":true,"mode":"inline"}})");
    const auto versionBefore = processing.getConfigVersion();
    {
        const std::string patch = imageProcessing(Json::parse(
            R"({"area_threshold_min":25,"area_threshold_max":400,"deformability_threshold_max":0.8,)"
            R"("ring_ratio_min":10.5,"area_ratio_threshold_max":2.5,)"
            R"("filters":{"enable_border_check":false,"enable_deformability_range_check":true,)"
            R"("enable_ring_ratio_check":false,"enable_area_ratio_check":true,"require_single_inner_contour":false},)"
            R"("target_group":{"enabled":true,"area_min":30,"area_max":150,"deformability_min":0.01,)"
            R"("deformability_max":0.25,"emodulus_enabled":true,"emodulus_min":1.5,"emodulus_max":8.25}})"));
        const auto result = tune(facade, patch);
        MIB_EXPECT(result.ok, "an Active run takes a tune patch: " + result.message);
        const auto c = processing.getProcessingConfig();
        MIB_EXPECT(c.area_threshold_min == 25 && c.area_threshold_max == 400 && c.deformability_threshold_max == 0.8 &&
                       c.ring_ratio_min == 10.5 && c.area_ratio_threshold_max == 2.5 && !c.enable_border_check &&
                       c.enable_deformability_range_check && !c.enable_ring_ratio_check && c.enable_area_ratio_check &&
                       !c.require_single_inner_contour && c.enable_target_group && c.target_group_area_min == 30 &&
                       c.target_group_area_max == 150 && c.target_group_deformability_min == 0.01 &&
                       c.target_group_deformability_max == 0.25 && c.enable_target_group_emodulus &&
                       c.target_group_emodulus_min == 1.5 && c.target_group_emodulus_max == 8.25,
                   "the tune patch reads back from the running configuration");
        MIB_EXPECT(c.gaussian_blur_size == 3 && c.morph_iterations == 1 && c.multi_image_count == 1 && !c.multi_image_enabled &&
                       c.enable_area_range_check && c.ring_ratio_max == processing.getProcessingConfig().ring_ratio_max,
                   "everything else in the running configuration stands");
        MIB_EXPECT(processing.getConfigVersion() > versionBefore, "the configuration version moved (the processing loop re-reads it)");
        const auto provenance = Json::parse(backend.getLastConfigJson());
        MIB_EXPECT(provenance["marker"]["keep"] == 7 && provenance["roi"]["x"] == 1 && provenance["roi"]["h"] == 4 &&
                       provenance["pixel_to_micron_factor"] == 9.5 && provenance["realtime_processing"]["enabled"] == true &&
                       provenance.size() == 5,
                   "the provenance text keeps its other keys as they were, and gains none");
        MIB_EXPECT(provenance["image_processing"]["area_threshold_min"] == 25 &&
                       provenance["image_processing"]["target_group"]["emodulus_max"] == 8.25,
                   "the provenance text carries the new criteria");
        std::string fetched;
        MIB_REQUIRE(facade.fetchProcessingConfigJson(fetched), "the live document reads");
        const auto live = Json::parse(fetched);
        MIB_EXPECT(live["image_processing"]["area_threshold_min"] == 25 && live["config_version"] == processing.getConfigVersion(),
                   "the live document shows the tuned value and the new version");
    }

    // ---- Active: everything else is refused, nothing changes -----------------------------------
    {
        const auto frozen = processing.getProcessingConfig();
        const auto frozenVersion = processing.getConfigVersion();
        const auto frozenProvenance = backend.getLastConfigJson();
        const auto roiBefore = processing.getRealtimeRoi();
        const std::vector<std::string> refused = {
            imageProcessing(Json{{"gaussian_blur_size", 5}}),
            imageProcessing(Json{{"multi_image", Json{{"enabled", true}, {"count", 4}}}}),
            imageProcessing(Json{{"multi_image", Json{{"count", 2}}}}),
            imageProcessing(Json{{"filters", Json{{"enable_laplacian_variance_check", true}}}}),
            imageProcessing(Json{{"auto_background_enabled", true}}),
            imageProcessing(Json{{"area_threshold_min", 10}, {"morph_iterations", 3}}),
            Json{{"image_processing", Json{{"area_threshold_min", 10}}}, {"realtime_processing", Json{{"enabled", false}}}}.dump(),
            Json{{"image_processing", Json{{"area_threshold_min", 10}}}, {"pixel_to_micron", 2.0}}.dump(),
            Json{{"image_processing", Json{{"area_threshold_min", 10}}}, {"roi", Json{{"x", 0}, {"y", 0}, {"w", 0}, {"h", 0}}}}.dump(),
            Json{{"image_processing", Json{{"area_threshold_min", 10}}}, {"flush_interval", 7}}.dump(),
            Json{{"area_threshold_min", 10}}.dump(),                // the bare section form
            Json{{"realtime_processing", Json{{"enabled", false}}}}.dump(),
            "not json",
        };
        for (const auto& patch : refused) {
            const auto result = tune(facade, patch);
            MIB_EXPECT(!result.ok && result.message.find("locked during an experiment") != std::string::npos,
                       "refused while Active: " + patch + " -> " + result.message);
        }
        bridge::ProcessingSettingsCommand typed;
        auto cfg = processing.getProcessingConfig();
        cfg.area_threshold_min = 11;
        typed.config = cfg;
        MIB_EXPECT(!facade.dispatch(typed).ok, "a typed config command is refused while Active");
        bridge::ProcessingSettingsCommand roi;
        roi.roi = backend::services::ProcessingService::Roi{0, 0, 0, 0};
        MIB_EXPECT(!facade.dispatch(roi).ok, "an ROI change is refused while Active");
        bridge::ProcessingSettingsCommand realtime;
        realtime.realtimeEnabled = false;
        MIB_EXPECT(!facade.dispatch(realtime).ok, "the realtime switch is refused while Active");
        bridge::ProcessingSettingsCommand pixel;
        pixel.pixelToMicronFactor = 2.0;
        MIB_EXPECT(!facade.dispatch(pixel).ok, "the pixel size is refused while Active");
        bridge::ProcessingSettingsCommand flush;
        flush.flushInterval = 9;
        MIB_EXPECT(!facade.dispatch(flush).ok, "the flush interval is refused while Active");
        bridge::ProcessingSettingsCommand empty;
        MIB_EXPECT(!facade.dispatch(empty).ok, "an empty command is refused while Active");

        // A tune patch that merges into an invalid range is refused whole.
        const auto inverted = tune(facade, imageProcessing(Json{{"area_threshold_min", 500}}));
        MIB_EXPECT(!inverted.ok, "min above max is refused while Active");
        const auto badType = tune(facade, imageProcessing(Json{{"area_threshold_min", "big"}}));
        MIB_EXPECT(!badType.ok, "a wrong type is refused while Active");
        const auto badFlag = tune(facade, imageProcessing(Json{{"filters", Json{{"enable_border_check", 1}}}}));
        MIB_EXPECT(!badFlag.ok, "a number for a switch is refused while Active");

        MIB_EXPECT(backend::processing::config_json::toJson(processing.getProcessingConfig()) ==
                       backend::processing::config_json::toJson(frozen),
                   "a refusal changed nothing in the configuration");
        MIB_EXPECT(processing.getConfigVersion() == frozenVersion && backend.getLastConfigJson() == frozenProvenance,
                   "a refusal changed neither the version nor the provenance");
        const auto roiAfter = processing.getRealtimeRoi();
        MIB_EXPECT(roiAfter.x == roiBefore.x && roiAfter.w == roiBefore.w, "a refusal left the ROI alone");
    }

    // ---- Active: the document path --------------------------------------------------------------
    {
        const auto path = (dataDir / "checked-config.json").string();
        // The document is stale against the running configuration: other block size, other area range.
        std::ofstream(path) << R"({"custom":{"keep":17},"image_processing":{"gaussian_blur_size":7,"area_threshold_min":12,)"
                               R"("area_threshold_max":2000,"unknown":3}})";
        auto baseline = facade.fetchConfigDocument(path);
        MIB_REQUIRE(baseline.ok, baseline.error);
        const auto runningBefore = processing.getProcessingConfig();

        // Refused: a non-tune key, nothing written.
        auto result = facade.applyConfigDocument(path, baseline.revision, R"({"image_processing":{"gaussian_blur_size":9}})");
        MIB_EXPECT(!result.saved && !result.applied && result.error.find("gaussian_blur_size") != std::string::npos,
                   "a non-tune key through the document is refused, naming it: " + result.error);
        result = facade.applyConfigDocument(path, baseline.revision, R"({"image_processing":{"multi_image":{"count":3}}})");
        MIB_EXPECT(!result.saved && !result.applied && result.error.find("multi_image") != std::string::npos,
                   "multi_image through the document is refused, naming it: " + result.error);
        result = facade.applyConfigDocument(path, baseline.revision,
                                            R"({"image_processing":{"area_threshold_min":40,"morph_kernel_size":5}})");
        MIB_EXPECT(!result.saved && !result.applied, "a mixed patch is refused whole");
        MIB_EXPECT(facade.fetchConfigDocument(path).revision == baseline.revision, "refusals never wrote the file");

        // Refused: an invalid merged range.
        result = facade.applyConfigDocument(path, baseline.revision, R"({"image_processing":{"deformability_threshold_min":0.95}})");
        MIB_EXPECT(!result.saved && !result.applied, "an inverted deformability range through the document is refused");
        MIB_EXPECT(facade.fetchConfigDocument(path).revision == baseline.revision, "an invalid patch never wrote the file");

        // Applied: the tune patch.
        result = facade.applyConfigDocument(
            path, baseline.revision,
            R"({"image_processing":{"deformability_threshold_min":0.05,"filters":{"enable_area_range_check":false},)"
            R"("target_group":{"area_max":175}}})");
        MIB_EXPECT(result.saved && result.applied && result.verified, "a tune patch through the document applies: " + result.error);
        const auto running = processing.getProcessingConfig();
        MIB_EXPECT(running.deformability_threshold_min == 0.05 && !running.enable_area_range_check && running.target_group_area_max == 175,
                   "the running configuration took exactly the patch");
        MIB_EXPECT(running.gaussian_blur_size == runningBefore.gaussian_blur_size &&
                       running.area_threshold_min == runningBefore.area_threshold_min &&
                       running.area_threshold_max == runningBefore.area_threshold_max,
                   "the document's stale values (block size 7, area 12..2000) did not reach the running configuration");
        const auto saved = facade.fetchConfigDocument(path);
        const auto json = Json::parse(saved.documentJson);
        MIB_EXPECT(json["custom"]["keep"] == 17 && json["image_processing"]["unknown"] == 3 &&
                       json["image_processing"]["gaussian_blur_size"] == 7 &&
                       json["image_processing"]["deformability_threshold_min"] == 0.05 &&
                       json["image_processing"]["filters"]["enable_area_range_check"] == false &&
                       json["image_processing"]["target_group"]["area_max"] == 175,
                   "the file got exactly the patch and kept everything else");
        const auto provenance = Json::parse(backend.getLastConfigJson());
        MIB_EXPECT(provenance["marker"]["keep"] == 7 && provenance["roi"]["x"] == 1 &&
                       provenance["image_processing"]["target_group"]["area_max"] == 175,
                   "the provenance follows the runtime configuration and keeps its other keys");

        // A stale baseline is a conflict, nothing applied.
        const auto versionBefore2 = processing.getConfigVersion();
        result = facade.applyConfigDocument(path, baseline.revision, R"({"image_processing":{"area_threshold_min":41}})");
        MIB_EXPECT(result.conflict && !result.saved && !result.applied, "a stale baseline is a conflict while Active");
        MIB_EXPECT(processing.getConfigVersion() == versionBefore2, "a conflict applied nothing");
    }
    MIB_EXPECT(!transitionAccepted.load(), "no tune was accepted from a Starting or Stopping status");

    // ---- Stop races a tuning thread -------------------------------------------------------------
    // The thread tunes until its first refusal after it got applies through: from then on the run is
    // being stopped or has stopped, and a later success would be a tune that raced the finalization.
    std::atomic<bool> hammerDone{false}, lateSuccess{false};
    std::atomic<int> accepted{0}, lastAccepted{0};
    std::thread hammer([&] {
        int i = 0;
        while (!hammerDone.load()) {
            const int value = 100 + (i++ % 150);
            const bool stoppingBefore = stoppingSeen.load();
            const auto result = tune(facade, imageProcessing(Json{{"target_group", Json{{"area_min", value}}}}));
            if (result.ok) {
                if (stoppingBefore) lateSuccess.store(true);
                lastAccepted.store(value);
                accepted.fetch_add(1);
            } else if (accepted.load() > 0) {
                hammerDone.store(true);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    MIB_REQUIRE(waitFor([&] { return accepted.load() >= 20; }, 10000), "the tuning thread gets applies through while Active");
    bridge::ExperimentCommand stop;
    stop.action = bridge::ExperimentCommandAction::Stop;
    const auto stopped = facade.dispatch(stop);
    MIB_EXPECT(stopped.ok, "stop is accepted while a thread tunes: " + stopped.message);
    MIB_REQUIRE(waitFor([&] { return facade.fetchExperimentStatus(status) && status.terminal; }, 30000), "the run finalizes");
    MIB_EXPECT(status.finalizationOk && status.state == app::ExperimentRunState::Idle, "the run finalized cleanly under a tuning thread");
    MIB_EXPECT(waitFor([&] { return hammerDone.load(); }, 5000), "the tuning thread was refused once the run stopped");
    hammerDone.store(true);
    hammer.join();
    MIB_EXPECT(!lateSuccess.load(), "no tune that began after the run showed Stopping was accepted");
    MIB_EXPECT(stoppingProbes.load() >= 1 && !transitionAccepted.load(), "the Stopping status was probed and refused");
    {
        const int last = lastAccepted.load();
        MIB_EXPECT(processing.getProcessingConfig().target_group_area_min == last,
                   "the running configuration holds the last tune that was accepted");
        const auto provenance = Json::parse(backend.getLastConfigJson());
        MIB_EXPECT(provenance["image_processing"]["target_group"]["area_min"] == last,
                   "so does the provenance text");
        // What the finalization wrote into the file is that same text: nothing slipped in between.
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(runPath), "the run's file reads");
        const auto recorded = reader.readScalarStringAttribute("/experiment_info", "config_json");
        MIB_REQUIRE(recorded.has_value(), "the file records the configuration");
        const auto document = Json::parse(*recorded);
        MIB_EXPECT(document["image_processing"]["target_group"]["area_min"] == last && document["marker"]["keep"] == 7,
                   "the file's recorded configuration is the last tune, and it kept the provenance's other keys");
        reader.closeFile();
    }

    // Idle again: the criteria and everything else are open, as before.
    {
        const auto result = tune(facade, imageProcessing(Json{{"multi_image", Json{{"count", 2}}}, {"gaussian_blur_size", 3}}));
        MIB_EXPECT(result.ok, "idle takes any key again: " + result.message);
    }
    facade.shutdown();
    backend.shutdown();
    return mib::test::exitCode();
}
