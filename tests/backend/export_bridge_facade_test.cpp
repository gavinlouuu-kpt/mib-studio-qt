#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/recording/Hdf5Service.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <fstream>
#include <thread>

using namespace backend::bridge;
using nlohmann::json;
namespace {
uint64_t hash(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    uint64_t value = 14695981039346656037ULL;
    char byte;
    while (in.get(byte))
        value = (value ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    return value;
}
json terminal(BackendFacade& facade, uint64_t id, bool reanalysis = false) {
    for (int i = 0; i < 1000; ++i) {
        auto status = json::parse(reanalysis ? facade.fetchReviewReanalysisStatusJson() : facade.fetchReviewExportStatusJson());
        if (status.value("operation_id", "") == std::to_string(id) &&
            status["state"] != "running" && facade.activeOperationCount() == 0) {
            // Allow worker to complete its final publication before next submit.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    MIB_REQUIRE(false, "export terminal bounded wait");
    return {};
}
} // namespace
int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir dir("export_bridge");
    const auto source = dir / "fixture.h5";
    {
        backend::services::Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(source.string()), "fixture open");
        MIB_REQUIRE(writer.initializeDatasets(), "fixture datasets");
        std::vector<backend::services::ProcessedFrame> frames(24);
        for (size_t i = 0; i < frames.size(); ++i) {
            frames[i].index = i;
            frames[i].timestampNs = 1000 + i;
            frames[i].originalImage = cv::Mat(32, 32, CV_8UC1, cv::Scalar(i));
            frames[i].processedImage = frames[i].originalImage.clone();
            frames[i].validation.isValid = true;
            frames[i].validation.objectCount = 1;
        }
        MIB_REQUIRE(writer.appendFrames(frames, {}), "fixture write");
        MIB_REQUIRE(writer.writeExperimentInfo(1000, 1023, 24, 0, {}, {0,0,32,32}), "fixture experiment info");
        writer.closeFile();
    }
    const auto originalHash = hash(source);
#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE", "mock");
#else
    setenv("MIB_CAMERA_MODE", "mock", 1);
#endif
    backend::AppBackend backend;
    BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(dir.path().string()), "initialize");
    RecordingLoadCommand load;
    load.filePath = source.string();
    MIB_REQUIRE(facade.dispatch(load).ok, "load fixture");
    const json request{{"output_root", dir.path().string()}, {"format", "all"}};
    MIB_REQUIRE(!facade.submitReviewExportJson("{bad").ok, "reject malformed request");
    for (int cycle = 0; cycle < 6; ++cycle) {
        watchdog.mark("export lifecycle");
        std::atomic<bool> cancelled{false};
        std::atomic<bool> duplicateRejected{false};
        facade.setEventSink([&](const BackendEvent& event) {
            if (auto op = std::get_if<OperationStatusEvent>(&event)) {
                if (op->kind == BackendOperationKind::Export &&
                    op->state == BackendOperationState::Progress && !cancelled.exchange(true)) {
                    duplicateRejected = !facade.submitReviewExportJson(request.dump()).ok;
                    facade.requestOperationCancel(op->operationId);
                }
            }
        });
        auto started = facade.submitReviewExportJson(request.dump());
        MIB_REQUIRE(started.ok, "cancel job accepted");
        auto status = terminal(facade, started.operationId);
        MIB_REQUIRE(status["state"] == "cancelled", "cancel not success");
        MIB_REQUIRE(duplicateRejected, "one export at a time");
        MIB_REQUIRE(status["final_path"] == "", "cancel never publishes final");
        MIB_REQUIRE(status["retained_partial_path"] == "", "cancel cleanup");
        facade.setEventSink({});
        MIB_REQUIRE(facade.dispatch(load).ok, "reopen after cancel");
        started = facade.submitReviewExportJson(request.dump());
        MIB_REQUIRE(started.ok, "roundtrip accepted");
        status = terminal(facade, started.operationId);
        MIB_REQUIRE(status["state"] == "completed", "roundtrip completed");
        MIB_REQUIRE(std::filesystem::exists(status["final_path"].get<std::string>()),
                    "published output");
        MIB_REQUIRE(status["images_exported"] == "24", "all images exported");
        MIB_REQUIRE(hash(source) == originalHash, "source immutable");
    }
    // Retained cancellation must advertise a visibly partial directory/manifest.
    facade.setEventSink([&](const BackendEvent& event) {
        if (auto op = std::get_if<OperationStatusEvent>(&event)) {
            if (op->kind == BackendOperationKind::Export &&
                op->state == BackendOperationState::Progress &&
                json::parse(facade.fetchReviewExportStatusJson()).value("phase", "") == "metadata")
                facade.requestOperationCancel(op->operationId);
        }
    });
    auto retainedRequest = request;
    retainedRequest["keep_partial_on_failure"] = true;
    auto retainedStart = facade.submitReviewExportJson(retainedRequest.dump());
    MIB_REQUIRE(retainedStart.ok, "retained cancel accepted");
    auto retained = terminal(facade, retainedStart.operationId);
    MIB_REQUIRE(retained["state"] == "cancelled", "retained cancellation terminal");
    auto partial = std::filesystem::path(retained["retained_partial_path"].get<std::string>());
    MIB_REQUIRE(partial.filename().string().find(".partial-") != std::string::npos,
                "partial clearly named");
    MIB_REQUIRE(std::filesystem::exists(partial / "export-failure.json"),
                "failure manifest retained");
    facade.setEventSink({});
    auto overwrite = request;
    overwrite["format"] = "metrics_csv";
    overwrite["explicit_destination"] = source.string();
    auto overwriteStart = facade.submitReviewExportJson(overwrite.dump());
    MIB_REQUIRE(overwriteStart.ok, "source overwrite checked by worker");
    MIB_REQUIRE(terminal(facade, overwriteStart.operationId)["state"] == "failed",
                "source overwrite refused");
    MIB_REQUIRE(hash(source) == originalHash, "source protected from explicit overwrite");
    auto bad = request;
    bad["output_root"] = source.string(); // file, not directory: deterministic I/O fault
    auto started = facade.submitReviewExportJson(bad.dump());
    MIB_REQUIRE(started.ok, "fault job accepted asynchronously");
    const auto failed = terminal(facade, started.operationId);
    MIB_REQUIRE(failed["state"] == "failed", "fault is never success");
    MIB_REQUIRE(failed["final_path"] == "", "fault never publishes final");
    MIB_REQUIRE(!failed["error"].get<std::string>().empty(), "fault detail retained");
    MIB_REQUIRE(json::parse(facade.fetchReviewExportStatusJson()) == failed,
                "terminal survives missed events");
    MIB_REQUIRE(hash(source) == originalHash, "fault source immutable");
    // Batch sources use independent readers; failures do not drop later files
    // or alter the currently loaded review source.
    auto batch = request;
    batch["format"] = "metrics_csv";
    batch["source_paths"] = json::array({source.string(), (dir / "missing.h5").string(), source.string()});
    auto batchStart = facade.submitReviewExportJson(batch.dump());
    MIB_REQUIRE(batchStart.ok, "batch accepted");
    auto batchResult = terminal(facade, batchStart.operationId);
    MIB_REQUIRE(batchResult["state"] == "failed", "partial batch is not success");
    MIB_REQUIRE(batchResult["results"].size() == 3, "all noncancelled files attempted");
    MIB_REQUIRE(batchResult["results"][0]["state"] == "completed", "first succeeds");
    MIB_REQUIRE(batchResult["results"][1]["state"] == "failed", "missing source reported");
    MIB_REQUIRE(batchResult["results"][2]["state"] == "completed", "later source still attempted");
    MIB_REQUIRE(batchResult["results"][0]["final_path"] != batchResult["results"][2]["final_path"], "duplicate names distinct");
    BackendReviewMetadata review;
    MIB_REQUIRE(facade.fetchReviewMetadata(review) && review.filePath == source.string(), "batch preserves current review");
    batch["source_paths"] = json::array();
    MIB_REQUIRE(!facade.submitReviewExportJson(batch.dump()).ok, "empty explicit batch rejected");
    batch["source_paths"] = json::array({source.string()});
    batch["explicit_destination"] = source.string();
    MIB_REQUIRE(!facade.submitReviewExportJson(batch.dump()).ok, "batch cannot target explicit shared destination");
    MIB_REQUIRE(hash(source) == originalHash, "batch sources unchanged");
    const auto regeneratedPath=dir / "regenerated.h5";
    json regenerate{{"source_path",source.string()},{"output_path",regeneratedPath.string()},
        {"dataset","/valid_frames/images"},{"start",0},{"count",0}};
    auto regeneration=facade.submitReviewReanalysisJson(regenerate.dump());
    MIB_REQUIRE(regeneration.ok,"reanalysis accepted");
    auto regenerationStatus=terminal(facade,regeneration.operationId,true);
    MIB_REQUIRE(regenerationStatus["state"]=="completed","reanalysis complete");
    {
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(regeneratedPath.string()),"regenerated output opens");
        backend::processing::ProcessingCoreIdentity identity;
        MIB_REQUIRE(reader.readProcessingCoreIdentity(identity),"output core identity recorded");
        std::vector<backend::services::ProcessedFrame> validFrames,invalidFrames;
        reader.readValidMetadata(validFrames);reader.readInvalidMetadata(invalidFrames);
        validFrames.insert(validFrames.end(),invalidFrames.begin(),invalidFrames.end());
        MIB_REQUIRE(validFrames.size()==24,"output source count preserved");
        for(const auto& frame:validFrames) MIB_REQUIRE(frame.timestampNs==1000+frame.index,"source timestamps preserved");
    }
    MIB_REQUIRE(!facade.submitReviewReanalysisJson(regenerate.dump()).ok,"existing output cannot be replaced");
    for(int cycle=0;cycle<4;++cycle) {
        regenerate["output_path"]=(dir / ("cancel-reanalysis-"+std::to_string(cycle)+".h5")).string();
        facade.setEventSink([&](const BackendEvent& event) {
            if(auto op=std::get_if<OperationStatusEvent>(&event)) {
                if(op->kind==BackendOperationKind::Reanalysis && op->state==BackendOperationState::Progress)
                    facade.requestOperationCancel(op->operationId);
            }
        });
        regeneration=facade.submitReviewReanalysisJson(regenerate.dump());
        MIB_REQUIRE(regeneration.ok,"cancel reanalysis accepted");
        MIB_REQUIRE(terminal(facade,regeneration.operationId,true)["state"]=="cancelled","cancelled reanalysis not success");
        MIB_REQUIRE(!std::filesystem::exists(regenerate["output_path"].get<std::string>()),"cancel no published output");
        facade.setEventSink({});
    }
    regenerate["source_path"]=(dir / "missing.h5").string();
    regeneration=facade.submitReviewReanalysisJson(regenerate.dump());
    MIB_REQUIRE(regeneration.ok,"invalid source checked asynchronously");
    MIB_REQUIRE(terminal(facade,regeneration.operationId,true)["state"]=="failed","read fault never success");
    regenerate["source_path"]=source.string();regenerate["output_path"]=(source / "output.h5").string();
    regeneration=facade.submitReviewReanalysisJson(regenerate.dump());
    MIB_REQUIRE(regeneration.ok,"write fault accepted asynchronously");
    MIB_REQUIRE(terminal(facade,regeneration.operationId,true)["state"]=="failed","write fault never success");
    MIB_REQUIRE(hash(source)==originalHash,"reanalysis source immutable");
    facade.shutdown();
    return mib::test::exitCode();
}
