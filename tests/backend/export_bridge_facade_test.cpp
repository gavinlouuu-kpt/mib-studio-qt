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
json terminal(BackendFacade& facade, uint64_t id) {
    for (int i = 0; i < 1000; ++i) {
        auto status = json::parse(facade.fetchReviewExportStatusJson());
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
            frames[i].originalImage = cv::Mat(32, 32, CV_8UC1, cv::Scalar(i));
            frames[i].processedImage = frames[i].originalImage.clone();
            frames[i].validation.isValid = true;
            frames[i].validation.objectCount = 1;
        }
        MIB_REQUIRE(writer.appendFrames(frames, {}), "fixture write");
        writer.closeFile();
    }
    const auto originalHash = hash(source);
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
    facade.shutdown();
    return mib::test::exitCode();
}
