#include "backend/processing/ProcessingService.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/review/ReviewSession.h"
#include "backend/playback/FrameStore.h"
#include "backend/recording/Hdf5Service.h"
#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <set>

int main() {
    mib::test::Watchdog watchdog(45);
    mib::test::TempDir directory("live_evidence");
    auto store = std::make_shared<backend::playback::FrameStore>(128);
    backend::services::ProcessingService processing;
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string error;
    MIB_REQUIRE(processing.activateProcessingKernel(kernel, &error), error);
    auto config = processing.getProcessingConfig();
    config.auto_background_enabled = false;
    config.empty_frame_pixel_threshold = 1;
    config.enable_ring_ratio_check = false;
    config.enable_border_check = false;
    config.area_threshold_min = 10;
    config.area_threshold_max = 100;
    processing.setPixelToMicronFactor(0.25);
    processing.setProcessingConfig(config);
    processing.setRealtimeRoi({0, 0, 96, 96});
    processing.setRealtimeDropFrames(false);
    processing.setInvalidFrameSamplingRate(1); // parity fixture retains both sets without sampling
    processing.setMonitoringActive(false); // evidence must not depend on UI monitoring
    processing.setRealtimeEnabled(true);
    processing.startRealtime(store);
    processing.setMonitoringRunGeneration(42);
    processing.startExperiment();
    kernel->mode = static_cast<int>(mib::test::FaultKernel::Mode::FailEmptyCheck);
    kernel->failPeriod = 3;
    for (uint64_t i = 0; i < 24; ++i) {
        auto frame =
            i % 2 ? mib::test::ringFrame(96, 96, i) : cv::Mat(96, 96, CV_8UC1, cv::Scalar(0));
        if (i % 4 == 3) {
            frame = cv::Mat(96, 96, CV_8UC1, cv::Scalar(0));
            mib::test::ringFrame(48, 48, i).copyTo(frame(cv::Rect(24, 24, 48, 48)));
        }
        store->pushFrame(frame.data, frame.total(), 96, 96, 96, 0x01080001, i, i);
    }
    MIB_EXPECT(processing.drainAndStopRealtime(), "all committed frames drain before stop");
    MIB_REQUIRE(processing.endExperiment(), "run drain completed");
    auto runJson = processing.runEvidenceJson();
    auto run = nlohmann::json::parse(runJson);
    MIB_REQUIRE(run.at("available").get<bool>(), "run archive available");
    MIB_EXPECT(run.at("finalized").get<bool>(), "run archive finalized");
    MIB_EXPECT(run.at("admittedFrames") == "23", "run-scoped admissions");
    MIB_EXPECT(!run.at("rows").empty(), "object rows reach recording archive");
    auto snapshot = processing.liveEvidence().snapshot();
    MIB_REQUIRE(snapshot.has_value(),
                "production processing publishes evidence with monitoring disabled");
    MIB_EXPECT(snapshot->finalized, "worker join finalizes evidence");
    // Existing processing cursor excludes index 0: name it, never weaken silently.
    MIB_EXPECT(snapshot->accounting.admittedFrames == 23, "frames [1,24) accounted");
    MIB_EXPECT(snapshot->accounting.errorFrames > 0, "kernel failures are visible");
    MIB_EXPECT(snapshot->accounting.completedFrames + snapshot->accounting.errorFrames +
                       snapshot->accounting.droppedFrames ==
                   23,
               "conserved terminal outcomes");
    const auto json = processing.liveEvidenceJson();
    const auto path = (directory.path() / "recording.h5").string();
    backend::services::Hdf5Service file;
    MIB_EXPECT(!file.writeLiveEvidenceJson(json), "unopened writer refuses evidence metadata");
    MIB_REQUIRE(file.openFile(path), "create evidence recording");
    MIB_REQUIRE(file.writeLiveEvidenceJson(json), "persist production evidence summary");
    MIB_REQUIRE(file.writeRunEvidenceJson(runJson), "persist run-scoped event rows");
    MIB_REQUIRE(file.initializeDatasets(), "initialize production recording datasets");
    MIB_REQUIRE(file.appendFrames(processing.getValidFrames(), processing.getInvalidFrames()),
                "persist the actual legacy run rows");
    file.closeFile();
    MIB_REQUIRE(file.loadFile(path), "reopen evidence recording");
    auto stored = file.readScalarStringAttribute("/monitoring", "live_evidence_json");
    MIB_REQUIRE(stored.has_value(), "saved summary present");
    MIB_EXPECT(nlohmann::json::parse(*stored) == nlohmann::json::parse(json), "summary round trip");
    MIB_EXPECT(!file.writeLiveEvidenceJson(json), "read-only recording refuses metadata mutation");
    auto storedRun = file.readScalarStringAttribute("/monitoring", "run_evidence_json");
    MIB_REQUIRE(storedRun.has_value(), "run event archive present");
    MIB_EXPECT(nlohmann::json::parse(*storedRun) == run,
               "run keys and rows survive recording round trip");
    MIB_EXPECT(!file.writeRunEvidenceJson(runJson), "read-only file refuses run archive mutation");
    std::vector<backend::services::ProcessedFrame> savedValid, savedInvalid;
    MIB_REQUIRE(file.readValidMetadata(savedValid), "read persisted valid scalar rows");
    MIB_REQUIRE(file.readInvalidMetadata(savedInvalid), "read persisted invalid scalar rows");
    std::multiset<std::string> legacyRows, evidenceRows;
    const auto addLegacy = [&](const auto& frames, const char* set) {
        for (const auto& frame : frames) {
            const auto& v = frame.validation;
            if (v.objectCount == 0 && v.objectId < 0) continue; // not an object event
            nlohmann::json key = {set, std::to_string(frame.index),
                                  v.objectId >= 0 ? nlohmann::json(std::to_string(v.objectId))
                                                  : nlohmann::json(nullptr),
                                  v.area, v.deformability};
            legacyRows.insert(key.dump());
        }
    };
    addLegacy(savedValid, "valid");
    addLegacy(savedInvalid, "invalid");
    for (const auto& row : run.at("rows")) {
        nlohmann::json key = {row.at("set"), row.at("frame"), row.at("object"),
                              row.at("values").at(0).at("value"),
                              row.at("values").at(1).at("value")};
        evidenceRows.insert(key.dump());
    }
    MIB_EXPECT(!savedValid.empty() && !savedInvalid.empty(), "both sets are nonempty");
    MIB_EXPECT(!legacyRows.empty(), "parity is not vacuous");
    MIB_EXPECT(
        legacyRows == evidenceRows,
        "saved legacy rows and evidence preserve set/lineage/scalars with sampling disabled");
    file.closeFile();
    // F consumes the real producer and the same persisted run, not reconstructed UI rows.
    const auto live = backend::analysis::freezeEvidence(processing.runLiveEvidence(), "live-run");
    backend::review::ReviewSession review;
    MIB_REQUIRE(review.open(path, &error), error);
    const auto archivedReply = nlohmann::json::parse(review.analyzePopulation(nlohmann::json{
        {"expectedPath", path}, {"feature", "area"}, {"minimum", nullptr}, {"maximum", nullptr},
        {"sourceKind", "run-archive"}, {"snapshot", true}}.dump()));
    MIB_REQUIRE(archivedReply.value("ok", false), archivedReply.dump());
    const auto archived = archivedReply.at("source");
    MIB_EXPECT(live.at("fingerprint") == archived.at("fingerprint"), "live/archive scientific fingerprint parity");
    MIB_EXPECT(live.at("rows") == archived.at("rows"), "live/archive keyed rows and image availability parity");
    const nlohmann::json graph = {{"id", "parity"}, {"revision", "1"}, {"output", "all"},
        {"nodes", {{{"id", "all"}, {"parent", ""}, {"kind", "interval"}, {"feature", "area"},
                    {"minimum", nullptr}, {"maximum", nullptr}}}}};
    const nlohmann::json evaluation = {{"graph", graph}, {"feature", "area"}};
    const auto liveResult = backend::analysis::evaluateSource(live, evaluation);
    MIB_EXPECT(liveResult == backend::analysis::evaluateSource(archived, evaluation), "live/archive masks and reductions parity");
    MIB_EXPECT(liveResult.at("denominator").is_null(), "processing failures retain unknown event denominator");
    for (const auto& row : live.at("rows"))
        MIB_EXPECT(row.at("image").at("available") == false, "event ordinals are not invented image locators");
    review.close();
    // Optional reproducible cross-consumer fixture from this real processing/HDF path.
    // No fixture rows are manufactured independently of the producer.
    if (const char* output = std::getenv("MIB_TEST_EVIDENCE_OUTPUT")) {
        std::filesystem::create_directories(output);
        std::filesystem::copy_file(path, std::filesystem::path(output) / "production-run.h5",
                                   std::filesystem::copy_options::overwrite_existing);
        std::ofstream(std::filesystem::path(output) / "production-run.json") << run.dump(2);
    }
    auto lease = processing.liveEvidence().acquireSnapshot(snapshot->identity);
    processing.setPixelToMicronFactor(0.25);
    MIB_EXPECT(!processing.liveEvidence().snapshot(), "calibration invalidates summary");
    MIB_EXPECT(processing.liveEvidence().page(snapshot->identity, lease.lease, 0, 10).status ==
                   backend::evidence::Status::Stale,
               "calibration invalidates pinned reads");
    processing.liveEvidence().release(lease.lease);
    // A second run on the same source must not reuse rows/keys from the first.
    kernel->failPeriod = 0;
    processing.setRealtimeProcessingMode(
        backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch);
    processing.startRealtime(store);
    processing.setMonitoringRunGeneration(43);
    processing.startExperiment();
    for (uint64_t i = 24; i < 27; ++i) {
        auto frame = mib::test::ringFrame(96, 96, i);
        if (i == 25) {
            frame = cv::Mat(96, 96, CV_8UC1, cv::Scalar(0));
            mib::test::ringFrame(48, 48, i).copyTo(frame(cv::Rect(24, 24, 48, 48)));
        }
        store->pushFrame(frame.data, frame.total(), 96, 96, 96, 0x01080001, i, i);
    }
    MIB_EXPECT(processing.drainAndStopRealtime(), "async run drains its workers");
    MIB_REQUIRE(processing.endExperiment(), "second run ended");
    auto second = nlohmann::json::parse(processing.runEvidenceJson());
    MIB_REQUIRE(second.at("available").get<bool>(), "second run archive available");
    MIB_EXPECT(second.at("sourceKey") != run.at("sourceKey"), "run identity never retargets");
    MIB_EXPECT(second.at("completedFrames") == "3" && second.at("droppedFrames") == "0",
               "realtime async returns every admitted frame, without offline track aggregation");
    MIB_EXPECT(second.at("admittedFrames") == "3",
               "second run excludes prior monitoring/run frames");
    MIB_EXPECT(processing.experimentAccountingSnapshot().admitted == 3,
               "legacy accounting uses the same run-start boundary");
    for (const auto& frame : processing.getValidFrames())
        MIB_EXPECT(frame.index >= 24, "legacy valid rows exclude previous run");
    for (const auto& frame : processing.getInvalidFrames())
        MIB_EXPECT(frame.index >= 24, "legacy invalid rows exclude previous run");
    for (const auto& row : second.at("rows"))
        MIB_EXPECT(std::stoull(row.at("frame").get<std::string>()) >= 24,
                   "only new source frames archived");

    MIB_EXPECT(second.at("coverage").at("eventCountKnown").get<bool>(),
               "clean run has a known event count");
    if (const char* output = std::getenv("MIB_TEST_EVIDENCE_OUTPUT")) {
        const auto cleanPath = (std::filesystem::path(output) / "clean-run.h5").string();
        MIB_REQUIRE(file.openFile(cleanPath), "create clean producer fixture");
        MIB_REQUIRE(file.initializeDatasets(), "initialize clean datasets");
        MIB_REQUIRE(file.appendFrames(processing.getValidFrames(), processing.getInvalidFrames()),
                    "save clean legacy rows");
        MIB_REQUIRE(file.writeRunEvidenceJson(second.dump()), "save clean archive");
        file.closeFile();
        std::ofstream(std::filesystem::path(output) / "clean-run.json") << second.dump(2);
    }
    auto replacement = std::make_shared<backend::playback::FrameStore>(8);
    processing.startRealtime(replacement);
    for (uint64_t i = 0; i < 3; ++i) {
        auto frame = mib::test::ringFrame(96, 96, i);
        replacement->pushFrame(frame.data, frame.total(), 96, 96, 96, 0x01080001, i, i);
    }
    MIB_EXPECT(processing.drainAndStopRealtime(), "replacement source drains");
    auto replacementEvidence = processing.liveEvidence().snapshot();
    MIB_REQUIRE(replacementEvidence.has_value(), "replacement source has a new evidence epoch");
    MIB_EXPECT(!(replacementEvidence->identity == snapshot->identity), "source epoch identity changes");
    MIB_EXPECT(replacementEvidence->accounting.admittedFrames == 2,
               "new source low indices are not fenced by the previous source cursor");
    return mib::test::exitCode();
}
