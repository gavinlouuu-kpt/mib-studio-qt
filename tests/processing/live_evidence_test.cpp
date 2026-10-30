#include "backend/analysis/EvidenceGallery.h"
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
    processing.setInvalidFrameSamplingRate(1);
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
    const auto liveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool liveImageObserved = false;
    while (std::chrono::steady_clock::now() < liveDeadline && !liveImageObserved) {
        auto current = processing.runLiveEvidence().snapshot();
        if (current && current->accounting.observedEvents) {
            auto held = processing.runLiveEvidence().acquireSnapshot(current->identity);
            auto page = processing.runLiveEvidence().page(current->identity, held.lease, 0, 1024);
            if (page.status == backend::evidence::Status::Ok && !page.rows.empty()) {
                const auto& row = page.rows.front();
                liveImageObserved = processing
                                        .readRunEvidenceImage(current->identity, held.lease,
                                                              row.set, std::to_string(row.ordinal))
                                        .status == "available";
                MIB_EXPECT(!current->finalized, "image read while run is live");
            }
            processing.runLiveEvidence().release(held.lease);
        }
        if (!liveImageObserved) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    MIB_EXPECT(liveImageObserved, "production live image lookup before drain");
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
    MIB_REQUIRE(file.initializeDatasets(), "initialize retained image datasets");
    MIB_REQUIRE(file.appendFrames(processing.getValidFrames(), processing.getInvalidFrames()),
                "persist actual both-set run buffers");
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
    auto runSnapshot = processing.runLiveEvidence().snapshot();
    MIB_REQUIRE(runSnapshot, "run identity for image lease");
    auto imageLease = processing.runLiveEvidence().acquireSnapshot(runSnapshot->identity);
    MIB_REQUIRE(imageLease.status == backend::evidence::Status::Ok, "image snapshot lease");
    for (const auto& row : run.at("rows")) {
        const auto set = row.at("set").get<std::string>();
        const auto ordinal = row.at("ordinal").get<std::string>();
        for (bool mask : {false, true}) {
            auto live = processing.readRunEvidenceImage(runSnapshot->identity, imageLease.lease,
                                                        set, ordinal, mask);
            auto recorded = file.readRunEvidenceImage(run.at("sourceKey"), set, ordinal, mask);
            MIB_REQUIRE(live.status == "available", "live exact image available: " + live.reason);
            MIB_REQUIRE(recorded.status == "available",
                        "recorded exact image available: " + recorded.reason);
            MIB_EXPECT(cv::norm(live.pixels, recorded.pixels, cv::NORM_INF) == 0,
                       "event image pixels agree despite set-specific HDF row order");
            live.pixels.setTo(cv::Scalar(255));
            auto again = processing.readRunEvidenceImage(runSnapshot->identity, imageLease.lease,
                                                         set, ordinal, mask);
            MIB_EXPECT(cv::norm(again.pixels, recorded.pixels, cv::NORM_INF) == 0,
                       "returned pixels do not mutate retained producer data");
        }
    }
    MIB_EXPECT(file.readRunEvidenceImage("wrong-source", "valid", "0").status == "refused",
               "archive source mismatch refused");
    MIB_EXPECT(file.readRunEvidenceImage(run.at("sourceKey"), "valid", "00").status == "refused",
               "noncanonical event ordinal refused");
    MIB_EXPECT(file.readRunEvidenceImage(run.at("sourceKey"), "valid", "9999").status == "missing",
               "absent event is missing, not HDF row index");
    processing.runLiveEvidence().release(imageLease.lease);
    MIB_EXPECT(
        processing.readRunEvidenceImage(runSnapshot->identity, imageLease.lease, "valid", "0")
                .status == "evicted",
        "released image lease refuses lookup");
    file.closeFile();
    // F consumes the real producer and the same persisted run, not reconstructed UI rows.
    const auto live = backend::analysis::freezeEvidence(processing.runLiveEvidence(), "live-run");
    backend::review::ReviewSession review;
    MIB_REQUIRE(review.open(path, &error), error);
    const auto archivedReply = nlohmann::json::parse(review.analyzePopulation(nlohmann::json{
        {"expectedPath", path},
        {"feature", "area"},
        {"minimum", nullptr},
        {"maximum", nullptr},
        {"sourceKind", "run-archive"},
        {"snapshot", true}}.dump()));
    MIB_REQUIRE(archivedReply.value("ok", false), archivedReply.dump());
    const auto archived = archivedReply.at("source");
    MIB_EXPECT(live.at("fingerprint") == archived.at("fingerprint"),
               "live/archive scientific fingerprint parity");
    MIB_EXPECT(live.at("rows") == archived.at("rows"),
               "live/archive keyed rows and image availability parity");
    const nlohmann::json graph = {{"id", "parity"},
                                  {"revision", "1"},
                                  {"output", "all"},
                                  {"nodes",
                                   {{{"id", "all"},
                                     {"parent", ""},
                                     {"kind", "interval"},
                                     {"feature", "area"},
                                     {"minimum", nullptr},
                                     {"maximum", nullptr}}}}};
    const nlohmann::json evaluation = {{"graph", graph}, {"feature", "area"}};
    const auto liveResult = backend::analysis::evaluateSource(live, evaluation);
    MIB_EXPECT(liveResult == backend::analysis::evaluateSource(archived, evaluation),
               "live/archive masks and reductions parity");
    MIB_EXPECT(liveResult.at("denominator").is_null(),
               "processing failures retain unknown event denominator");
    for (const auto& row : live.at("rows"))
        MIB_EXPECT(row.at("image").at("available") == false,
                   "event ordinals are not invented image locators");
    // Scientific bindings made before the optional retained-image provider stay valid.
    nlohmann::json legacyProjection;
    for (const auto* field : {"sourceKey", "coverage", "parameters", "rows", "gaps"})
        legacyProjection[field] = live.at(field);
    for (auto& row : legacyProjection["rows"])
        row["image"]["reason"] = "E provider has no retained-image locator";
    MIB_EXPECT(backend::analysis::contentHash(legacyProjection) == live.at("fingerprint"),
               "retained image support preserves pre-provider scientific fingerprints");
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& row : live.at("rows"))
        keys.push_back(row.at("key"));
    for (bool mask : {false, true}) {
        nlohmann::json liveGallery;
        backend::analysis::freezeEvidence(
            processing.runLiveEvidence(), "live-run",
            [&](const backend::evidence::Identity& identity, uint64_t held,
                const nlohmann::json& frozen) {
                liveGallery = backend::analysis::evidenceGallery(
                    frozen, keys, mask, [&](const std::string& set, const std::string& ordinal) {
                        return processing.readRunEvidenceImage(identity, held, set, ordinal, mask);
                    });
            });
        nlohmann::json galleryRequest{{"expectedPath", path},
                                      {"feature", "area"},
                                      {"minimum", nullptr},
                                      {"maximum", nullptr},
                                      {"sourceKind", "run-archive"},
                                      {"snapshot", true},
                                      {"galleryKeys", keys},
                                      {"expectedFingerprint", archived.at("fingerprint")},
                                      {"expectedFileFingerprint", archived.at("fileFingerprint")},
                                      {"mask", mask}};
        const auto archiveGallery =
            nlohmann::json::parse(review.analyzePopulation(galleryRequest.dump()));
        MIB_REQUIRE(archiveGallery.value("ok", false), archiveGallery.dump());
        MIB_EXPECT(archiveGallery["source"]["images"] == liveGallery["images"],
                   "F live/archive exact thumbnails and masks match");
        for (const auto& image : liveGallery["images"])
            MIB_EXPECT(image["status"] == "available" && image["pixels"].size() == 4096,
                       "provider image actually rendered as thumbnail");
        auto invalidKeys = galleryRequest;
        invalidKeys["galleryKeys"] = nlohmann::json::array({keys.front(), keys.front()});
        MIB_EXPECT(
            !nlohmann::json::parse(review.analyzePopulation(invalidKeys.dump())).value("ok", true),
            "duplicate archive gallery keys refused");
        invalidKeys["galleryKeys"] = nlohmann::json::array({"[\"foreign\",\"valid\",\"0\"]"});
        MIB_EXPECT(
            !nlohmann::json::parse(review.analyzePopulation(invalidKeys.dump())).value("ok", true),
            "foreign archive gallery keys refused");
        galleryRequest["expectedFileFingerprint"] = "sha256:wrong";
        MIB_EXPECT(!nlohmann::json::parse(review.analyzePopulation(galleryRequest.dump()))
                        .value("ok", true),
                   "archive image file fingerprint mismatch refused");
    }
    review.close();
    auto lease = processing.liveEvidence().acquireSnapshot(snapshot->identity);
    const auto calibratedLease =
        processing.runLiveEvidence().acquireSnapshot(runSnapshot->identity);
    MIB_REQUIRE(calibratedLease.status == backend::evidence::Status::Ok,
                "image lease before calibration");
    processing.setPixelToMicronFactor(0.25);
    MIB_EXPECT(
        processing.readRunEvidenceImage(runSnapshot->identity, calibratedLease.lease, "valid", "0")
                .status == "evicted",
        "calibration invalidates image lease");
    processing.runLiveEvidence().release(calibratedLease.lease);
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
        store->pushFrame(frame.data, frame.total(), 96, 96, 96, 0x01080001, i, i);
    }
    MIB_EXPECT(processing.drainAndStopRealtime(), "async run drains its workers");
    MIB_REQUIRE(processing.endExperiment(), "second run ended");
    auto second = nlohmann::json::parse(processing.runEvidenceJson());
    MIB_REQUIRE(second.at("available").get<bool>(), "second run archive available");
    MIB_EXPECT(second.at("sourceKey") != run.at("sourceKey"), "run identity never retargets");
    MIB_EXPECT(second.at("admittedFrames") == "3",
               "second run excludes prior monitoring/run frames");
    for (const auto& row : second.at("rows"))
        MIB_EXPECT(std::stoull(row.at("frame").get<std::string>()) >= 24,
                   "only new source frames archived");

    return mib::test::exitCode();
}
