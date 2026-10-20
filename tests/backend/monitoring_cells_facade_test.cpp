// monitoring_cells_facade_test (#716 A6a, bridge ABI 41)
//
// Live cell images through the BackendFacade surface the bridge calls, with
// the real AppBackend, ExperimentCoordinator and monitoring ring (mock camera;
// rows enter the ring through ProcessingService's append seam, which tags them
// exactly as the realtime path does):
//   1. snapshot rows carry their run's start generation (0 outside a run, the
//      run's while it accumulates, 0 again after it stops);
//   2. the listing JSON (strip, pins, caps, stats) and the image pull agree:
//      the packet's size is the listed crop's, its pixels are the row's;
//   3. a camera session opening and a realtime start are new captures: an
//      earlier capture's identity is CaptureMismatch, its cells leave the
//      strip and its pins keep their measurements only; Start is a capture
//      change too and drops the pins and the strip; a pin of the previous run
//      is refused; a pre-run row is not the run's;
//   4. a row the ring evicted after a snapshot served it is pinned with its
//      measurements and the reason, and no image is served for it;
//   5. monitoring_clear clears the strip and keeps the pins; unpin and clear;
//   6. capture changes take effect at the moment they happen (review rounds
//      2-3), each with a strip cell and a pin of the old capture: another
//      processing ROI, realtime stopping while the camera keeps running, the
//      camera stopping, a frame buffer resize, and a MindVision switch to
//      Overview with realtime stopped (a new frame buffer, no frame processed
//      after it). At once, with no frame and no refresh in between, the old
//      identities are refused for pin (CaptureMismatch) and image (with the
//      reason), the strip is empty, the pin is listed with its measurements
//      only ("capture E ended (reason)"), the listing says why, and unpin
//      removes the ended pin.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/MonitoringCellStore.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace {
namespace bridge = backend::bridge;
namespace app = backend::app;
using backend::services::MonitoringCellStore;
using backend::services::ProcessedFrame;
using json = nlohmann::json;

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

constexpr std::uint64_t kBase = 1000000000ull; // injected frame indexes, far from the mock camera's

// A row as the realtime path appends it: images shared, a box inside them.
void inject(backend::services::ProcessingService& proc, std::uint64_t frameIndex, bool valid, std::uint8_t value)
{
    ProcessedFrame f;
    f.index = frameIndex;
    f.timestampNs = frameIndex * 10;
    f.originalImage = cv::Mat(96, 512, CV_8UC1, cv::Scalar(value));
    f.processedImage = cv::Mat(96, 512, CV_8UC1, cv::Scalar(0));
    f.processedImage(cv::Rect(200, 30, 24, 20)).setTo(255);
    f.validation.isValid = valid;
    f.validation.objectId = 0;
    f.validation.objectCount = 1;
    f.validation.bboxX = 200;
    f.validation.bboxY = 30;
    f.validation.bboxWidth = 24;
    f.validation.bboxHeight = 20;
    f.validation.centroidX = 212;
    f.validation.centroidY = 40;
    f.validation.area = 400.0 + static_cast<double>(frameIndex % 1000);
    f.validation.deformability = 0.04;
    proc.appendMonitoringFrameForTests(f);
}

// An async-batch row: an ROI crop of the frame, its geometry in full-frame coordinates, tagged
// with the ROI's origin. The object (255) sits at (200, 30) in the crop.
void injectBatch(backend::services::ProcessingService& proc, std::uint64_t frameIndex, cv::Point roiOrigin)
{
    ProcessedFrame f;
    f.index = frameIndex;
    f.timestampNs = frameIndex * 10;
    f.originalImage = cv::Mat(96, 512, CV_8UC1, cv::Scalar(5));
    f.originalImage(cv::Rect(200, 30, 24, 20)).setTo(255);
    f.processedImage = cv::Mat(96, 512, CV_8UC1, cv::Scalar(0));
    f.processedImage(cv::Rect(200, 30, 24, 20)).setTo(255);
    f.validation.isValid = true;
    f.validation.objectId = 0;
    f.validation.objectCount = 1;
    f.validation.bboxX = 200 + roiOrigin.x;
    f.validation.bboxY = 30 + roiOrigin.y;
    f.validation.bboxWidth = 24;
    f.validation.bboxHeight = 20;
    f.validation.centroidX = 212 + roiOrigin.x;
    f.validation.centroidY = 40 + roiOrigin.y;
    f.validation.area = 480.0;
    proc.appendMonitoringFrameForTests(f, roiOrigin);
}

json cells(bridge::BackendFacade& facade)
{
    std::string text;
    MIB_REQUIRE(facade.fetchMonitoringCellsJson(text), "fetch_monitoring_cells");
    return json::parse(text);
}

const json* inList(const json& list, std::uint64_t frameIndex)
{
    for (const auto& c : list)
        if (c["frame_index"] == std::to_string(frameIndex)) return &c;
    return nullptr;
}

json pin(bridge::BackendFacade& facade, std::uint64_t generation, std::uint64_t epoch, std::uint64_t frameIndex)
{
    return json::parse(facade.pinMonitoringCellJson(generation, epoch, frameIndex, 0));
}

std::optional<bridge::MonitoringObjectRow> snapshotRow(bridge::BackendFacade& facade, std::uint64_t frameIndex)
{
    bridge::BackendMonitoringSnapshot s;
    MIB_REQUIRE(facade.fetchMonitoringSnapshot(s, 4000), "snapshot");
    for (const auto& r : s.rows)
        if (r.frameIndex == frameIndex) return r;
    return std::nullopt;
}

std::optional<std::string> snapshotGeneration(bridge::BackendFacade& facade, std::uint64_t frameIndex)
{
    const auto r = snapshotRow(facade, frameIndex);
    if (!r) return std::nullopt;
    return std::to_string(r->startGeneration);
}

bridge::BackendCommandResult startRun(bridge::BackendFacade& facade, const std::string& path)
{
    app::ExperimentReadinessSnapshot readiness;
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
    if (facade.fetchExperimentReadiness(readiness, path)) start.readinessGeneration = readiness.generation;
    return facade.dispatch(start);
}

int outcome(const json& reply) { return reply.contains("outcome") ? reply["outcome"].get<int>() : -1; }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A strip cell and a pin of the current capture (outside a run): rows `first` and `first + 1`.
std::uint64_t holdTwo(bridge::BackendFacade& facade, backend::services::ProcessingService& proc, std::uint64_t first)
{
    const auto epoch = proc.monitoringCaptureEpoch();
    inject(proc, first, true, 30);
    inject(proc, first + 1, true, 31);
    const auto l = cells(facade);
    MIB_REQUIRE(inList(l["strip"], first) && inList(l["strip"], first + 1), "both rows are strip cells: " + l.dump());
    MIB_REQUIRE(outcome(pin(facade, 0, epoch, first + 1)) == 0, "pin the second");
    bridge::BackendFrame frame;
    MIB_REQUIRE(facade.fetchMonitoringCellImage(0, epoch, first, 0, false, frame) &&
                    facade.fetchMonitoringCellImage(0, epoch, first + 1, 0, false, frame),
                "both images are served before the change");
    return epoch;
}

// Right after a capture change: nothing of the old capture is served or pinned, the strip is
// empty, and the pin is listed with its measurements only.
void expectEnded(bridge::BackendFacade& facade, backend::services::ProcessingService& proc, std::uint64_t first,
                 std::uint64_t oldEpoch, const std::string& reason, const std::string& what)
{
    const auto now = proc.monitoringCaptureEpoch();
    MIB_EXPECT(now > oldEpoch, what + ": the capture moved on at once");
    bridge::BackendFrame frame;
    for (const std::uint64_t f : {first, first + 1}) { // the strip cell, then the pin
        for (const bool mask : {false, true}) {
            bool mismatch = false;
            std::string why;
            MIB_EXPECT(!facade.fetchMonitoringCellImage(0, oldEpoch, f, 0, mask, frame, &mismatch, &why) && mismatch &&
                           contains(why, reason),
                       what + ": the old " + (f == first ? "strip" : "pinned") + (mask ? " mask" : " image") +
                           " is refused: " + why);
        }
        const auto r = pin(facade, 0, oldEpoch, f);
        MIB_EXPECT(outcome(r) == 6 && r["ok"] == false && contains(r["message"].get<std::string>(), reason),
                   what + ": the old identity is CaptureMismatch: " + r.dump());
    }
    std::string text;
    MIB_REQUIRE(facade.fetchMonitoringCellsJson(text), "listing");
    auto l = json::parse(text);
    MIB_EXPECT(l["strip"].empty() && l["stats"]["strip_cells"] == 0 && l["stats"]["pin_bytes"] == "0" &&
                   l["ring_capture_epoch"] == std::to_string(now),
               what + ": the strip holds nothing of the old capture: " + l.dump());
    MIB_EXPECT(l["pins"].size() == 1 && l["pins"][0]["capture_epoch"] == std::to_string(oldEpoch) &&
                   l["pins"][0]["image"].is_null() &&
                   contains(l["pins"][0]["no_image_reason"].get<std::string>(),
                            "capture " + std::to_string(oldEpoch) + " ended (") &&
                   contains(l["pins"][0]["no_image_reason"].get<std::string>(), reason),
               what + ": the pin keeps its measurements only: " + l.dump());
    const auto& change = l["last_capture_change"];
    MIB_EXPECT(change.is_object() && change["capture_epoch"] == std::to_string(now) &&
                   contains(change["reason"].get<std::string>(), reason) && change["pins_ended"] == 1 &&
                   change["pins_dropped"] == 0 && change["strip_dropped"].get<int>() >= 2,
               what + ": the listing says why: " + l.dump());
    const auto u = facade.unpinMonitoringCell(0, oldEpoch, first + 1, 0);
    MIB_EXPECT(u.ok && u.message == "unpinned", what + ": unpin removes the ended pin: " + u.message);
    MIB_REQUIRE(facade.fetchMonitoringCellsJson(text), "listing");
    l = json::parse(text);
    MIB_EXPECT(l["pins"].empty(), what + ": unpinned: " + l.dump());
}

} // namespace

int main()
{
    mib::test::Watchdog wd(90);
    mib::test::TempDir td("mib_monitoring_cells");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 512, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        {
            std::string text;
            MIB_EXPECT(!facade.fetchMonitoringCellsJson(text), "nothing before initialize");
            MIB_EXPECT(!json::parse(facade.pinMonitoringCellJson(0, 1, 1, 0))["ok"].get<bool>(), "pin before init");
        }
        MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
        auto& proc = backendApp.processing();

        // ---- 1-2. outside a run: tags, listing, image pull ------------------------------------
        wd.mark("outside a run");
        for (std::uint64_t i = 1; i <= 3; ++i) inject(proc, kBase + i, true, static_cast<std::uint8_t>(10 * i));
        inject(proc, kBase + 4, false, 99);
        MIB_EXPECT(snapshotGeneration(facade, kBase + 1) == std::string("0"), "a row outside a run is generation 0");
        const auto e1 = proc.monitoringCaptureEpoch();
        MIB_EXPECT(snapshotRow(facade, kBase + 1)->captureEpoch == e1 && e1 >= 1, "rows carry the capture epoch");
        auto l = cells(facade);
        MIB_EXPECT(l["valid"] == true && l["scope_start_generation"] == "0" && l["ring_start_generation"] == "0",
                   l.dump());
        MIB_EXPECT(l["caps"]["strip_max_cells"] == MonitoringCellStore::kStripMaxCells &&
                       l["caps"]["max_pins"] == MonitoringCellStore::kMaxPins &&
                       l["caps"]["crop_max_side"] == MonitoringCellStore::kCropMaxSide,
                   "caps are reported");
        MIB_REQUIRE(l["strip"].size() == 3, "three valid cells in the strip: " + l.dump());
        MIB_EXPECT(!inList(l["strip"], kBase + 4), "an invalid row is not a strip cell");
        const auto& cell = *inList(l["strip"], kBase + 2);
        MIB_EXPECT(cell["start_generation"] == "0" && cell["capture_epoch"] == std::to_string(e1) &&
                       l["ring_capture_epoch"] == std::to_string(e1) && cell["object_id"] == 0 && cell["valid"] == true &&
                       cell["no_image_reason"].is_null() && cell["image"]["has_mask"] == true,
                   cell.dump());
        // box 24 x 20, margin max(8, 12) = 12: 48 x 44 around the box.
        MIB_EXPECT(cell["image"]["width"] == 48 && cell["image"]["height"] == 44 && cell["image"]["crop_x"] == 188 &&
                       cell["image"]["crop_y"] == 18 && cell["image"]["source_width"] == 512,
                   cell["image"].dump());
        bridge::BackendFrame frame;
        MIB_REQUIRE(facade.fetchMonitoringCellImage(0, e1, kBase + 2, 0, false, frame), "image pull");
        MIB_EXPECT(frame.width == 48 && frame.height == 44 && frame.strideBytes == 48 && frame.data.size() == 48u * 44 &&
                       frame.frameIndex == kBase + 2 && frame.pixelFormat == 0,
                   "the packet's geometry is the listed crop's");
        MIB_EXPECT(frame.data[0] == 20 && frame.data[frame.data.size() - 1] == 20, "the row's pixels");
        MIB_REQUIRE(facade.fetchMonitoringCellImage(0, e1, kBase + 2, 0, true, frame), "mask pull");
        std::size_t marked = 0;
        for (auto b : frame.data) marked += b == 255;
        MIB_EXPECT(marked == 24u * 20, "the mask crop marks the object");
        bool mismatch = true;
        std::string why;
        MIB_EXPECT(!facade.fetchMonitoringCellImage(0, e1, kBase + 4, 0, false, frame, &mismatch, &why) && !mismatch &&
                       contains(why, "not held"),
                   "not held: no packet, " + why);
        MIB_EXPECT(!facade.fetchMonitoringCellImage(0, e1 + 1, kBase + 2, 0, false, frame, &mismatch, &why) && mismatch,
                   "another capture: refused, " + why);

        auto r = pin(facade, 0, e1, kBase + 1);
        MIB_EXPECT(r["ok"] == true && outcome(r) == 0 && r["cell"]["frame_index"] == std::to_string(kBase + 1),
                   r.dump());
        r = pin(facade, 0, e1, kBase + 1);
        MIB_EXPECT(r["ok"] == true && outcome(r) == 2, "AlreadyPinned: " + r.dump());
        r = pin(facade, 0, e1, kBase + 4); // an invalid row may be pinned too (from the scatter or the table)
        MIB_EXPECT(r["ok"] == true && outcome(r) == 0, r.dump());

        // An async-batch row (geometry measured on the full frame, ROI origin (150, 8)): the
        // snapshot and the cell report ROI coordinates, and the crop holds the object.
        injectBatch(proc, kBase + 6, {150, 8});
        const auto batchRow = snapshotRow(facade, kBase + 6);
        MIB_REQUIRE(batchRow, "the batch row is in the snapshot");
        MIB_EXPECT(batchRow->centroidX == 212 && batchRow->centroidY == 40, "snapshot centroid in ROI coordinates");
        l = cells(facade);
        const auto* batchCell = inList(l["strip"], kBase + 6);
        MIB_REQUIRE(batchCell, "the batch row is a strip cell: " + l.dump());
        MIB_EXPECT((*batchCell)["bbox"]["x"] == 200.0 && (*batchCell)["bbox"]["y"] == 30.0 &&
                       (*batchCell)["centroid_x"] == 212.0 && (*batchCell)["image"]["crop_x"] == 188,
                   "cell geometry in ROI coordinates: " + batchCell->dump());
        MIB_REQUIRE(facade.fetchMonitoringCellImage(0, e1, kBase + 6, 0, false, frame), "batch crop");
        std::size_t object = 0;
        for (auto b : frame.data) object += b == 255;
        MIB_EXPECT(object == 24u * 20, "the crop holds the whole object");
        r = pin(facade, 0, e1, kBase + 6);
        MIB_EXPECT(r["ok"] == true && outcome(r) == 0, r.dump());

        // ---- 3. a run: tags, pins dropped, stale refused ---------------------------------------
        wd.mark("run 1");
        {
            bridge::CameraCommand configure;
            configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
            configure.mockFrameDirectory = mockDir.string();
            configure.mockFrameIntervalMs = 5;
            configure.mockLoopFiles = true;
            MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
            bridge::CameraCommand startCapture;
            startCapture.action = bridge::CameraCommandAction::StartCapture;
            MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");
            const auto probe = (td.path() / "probe.h5").string();
            MIB_REQUIRE(waitFor(
                            [&] {
                                app::ExperimentReadinessSnapshot readiness;
                                return facade.fetchExperimentReadiness(readiness, probe) && readiness.ready;
                            },
                            std::chrono::seconds(15)),
                        "readiness passes with a running mock camera");
        }
        // The camera opened a capture session (review round 2): the earlier capture's cells are gone.
        // (Readiness may pass a moment before the capture thread reports the session open.)
        MIB_EXPECT(waitFor(
                       [&] {
                           const auto c = cells(facade);
                           return c["last_capture_change"].is_object() &&
                                  c["last_capture_change"]["reason"] == "the camera started a new capture session";
                       },
                       std::chrono::seconds(5)),
                   "a camera session opening is a capture change");
        l = cells(facade);
        bool allEnded = l["pins"].size() == 3;
        for (const auto& p : l["pins"])
            allEnded = allEnded && p["image"].is_null() && p["capture_epoch"] == std::to_string(e1) &&
                       contains(p["no_image_reason"].get<std::string>(),
                                "ended (the camera started a new capture session)");
        MIB_EXPECT(proc.monitoringCaptureEpoch() > e1 && allEnded && l["last_capture_change"]["pins_ended"] == 3 &&
                       l["strip"].empty(),
                   "the opening ended the earlier capture's pins (measurements only): " + l.dump());
        // The realtime loop starts on the capture: a new capture epoch. Rows from before it are an
        // earlier capture (their frame indices could recur), so they can no longer be pinned.
        proc.setRealtimeEnabled(true);
        proc.startRealtime(backendApp.getFrameStore());
        const auto e2 = proc.monitoringCaptureEpoch();
        MIB_EXPECT(e2 > e1, "a realtime start is a new capture");
        r = pin(facade, 0, e1, kBase + 3);
        MIB_EXPECT(outcome(r) == 6 && r["ok"] == false && r["cell"].is_null(),
                   "an earlier capture's identity is CaptureMismatch: " + r.dump());
        r = pin(facade, 0, e2, kBase + 3);
        MIB_EXPECT(outcome(r) == 4, "the current capture never saw that frame: " + r.dump());
        MIB_EXPECT(cells(facade)["strip"].empty(), "the strip keeps only the current capture's cells");
        inject(proc, kBase + 5, true, 40); // outside a run, this capture
        auto s1 = startRun(facade, (td.path() / "run1.h5").string());
        MIB_REQUIRE(s1.ok, "run 1 starts: " + s1.message);
        const auto run1 = backendApp.experiment().status().startGeneration;
        MIB_REQUIRE(run1 >= 1, "a start generation");
        MIB_EXPECT(proc.monitoringRunGeneration() == run1, "the ring tags rows with the run");
        l = cells(facade);
        MIB_EXPECT(l["pins"].empty() && l["scope_start_generation"] == std::to_string(run1),
                   "Start dropped the pins: " + l.dump());
        MIB_EXPECT(l["strip"].empty(), "pre-run cells are not the run's strip: " + l.dump());
        const auto e3 = proc.monitoringCaptureEpoch();
        MIB_EXPECT(e3 > e2 && l["last_capture_change"]["reason"] == "a run started" &&
                       l["last_capture_change"]["pins_dropped"] == 3 && l["last_capture_change"]["pins_ended"] == 0,
                   "a run start is a capture change that drops the (ended) pins: " + l.dump());
        inject(proc, kBase + 10, true, 50);
        MIB_EXPECT(snapshotGeneration(facade, kBase + 10) == std::to_string(run1), "a run row carries the run");
        MIB_EXPECT(snapshotGeneration(facade, kBase + 1) == std::string("0"), "a pre-run row keeps 0");
        l = cells(facade);
        MIB_EXPECT(l["strip"].size() == 1 && l["strip"][0]["start_generation"] == std::to_string(run1), l.dump());
        r = pin(facade, run1, e3, kBase + 10);
        MIB_EXPECT(outcome(r) == 0, "the run's row pins: " + r.dump());
        r = pin(facade, run1, e2, kBase + 5);
        MIB_EXPECT(outcome(r) == 6 && r["ok"] == false &&
                       r["message"].get<std::string>().find("a run started") != std::string::npos,
                   "a pre-run row is of the capture before the run: " + r.dump());
        r = pin(facade, run1, e3, kBase + 5);
        MIB_EXPECT(outcome(r) == 4, "and is no row of the run's capture: " + r.dump());
        r = pin(facade, run1 + 1, e3, kBase + 10);
        MIB_EXPECT(outcome(r) == 3, "a future run is refused: " + r.dump());

        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        stop.expectedStartGeneration = run1;
        MIB_REQUIRE(facade.dispatch(stop).ok, "stop run 1");
        MIB_REQUIRE(waitFor([&] { return backendApp.experiment().status().state == app::ExperimentRunState::Idle; },
                            std::chrono::seconds(20)),
                    "run 1 finalizes");
        MIB_EXPECT(proc.monitoringRunGeneration() == 0, "after the run, rows are outside a run");
        inject(proc, kBase + 11, true, 60);
        MIB_EXPECT(snapshotGeneration(facade, kBase + 11) == std::string("0"), "a post-run row is generation 0");
        l = cells(facade);
        MIB_EXPECT(l["pins"].size() == 1 && l["pins"][0]["frame_index"] == std::to_string(kBase + 10),
                   "pins outlive the run's end: " + l.dump());
        MIB_EXPECT(proc.monitoringCaptureEpoch() == e3, "a run's end is no capture change (realtime keeps running)");
        MIB_EXPECT(facade.fetchMonitoringCellImage(run1, e3, kBase + 10, 0, false, frame) && frame.data[0] == 50,
                   "the run's pin still serves its image");

        wd.mark("run 2");
        auto s2 = startRun(facade, (td.path() / "run2.h5").string());
        MIB_REQUIRE(s2.ok, "run 2 starts: " + s2.message);
        const auto run2 = backendApp.experiment().status().startGeneration;
        MIB_EXPECT(run2 > run1, "a newer generation");
        l = cells(facade);
        MIB_EXPECT(l["pins"].empty() && l["last_capture_change"]["pins_dropped"] == 1,
                   "the next Start drops run 1's pins: " + l.dump());
        const auto e4 = proc.monitoringCaptureEpoch();
        MIB_EXPECT(e4 > e3, "run 2's capture");
        r = pin(facade, run1, e3, kBase + 10);
        MIB_EXPECT(outcome(r) == 6, "run 1's capture ended: " + r.dump());
        r = pin(facade, run1, e4, kBase + 10);
        MIB_EXPECT(outcome(r) == 3 && r["message"].get<std::string>().find("not the latest run") != std::string::npos,
                   "a stale client cannot pin into the new run: " + r.dump());

        // ---- 4. evicted after a snapshot served it ---------------------------------------------
        wd.mark("evicted");
        inject(proc, kBase + 20, true, 70);
        MIB_REQUIRE(snapshotGeneration(facade, kBase + 20) == std::to_string(run2), "served by a snapshot");
        for (std::uint64_t i = 0; i < 1001; ++i) inject(proc, kBase + 100000 + i, true, 1); // the ring turns over
        r = pin(facade, run2, e4, kBase + 20);
        MIB_EXPECT(outcome(r) == 1 && r["ok"] == true && r["cell"]["image"].is_null(), "metadata only: " + r.dump());
        MIB_EXPECT(r["cell"]["area"] == 420.0 && r["cell"]["frame_index"] == std::to_string(kBase + 20),
                   "the measurements the snapshot reported");
        MIB_EXPECT(r["message"] == r["cell"]["no_image_reason"] &&
                       r["message"].get<std::string>().find("left the monitoring buffer") != std::string::npos,
                   "a plain reason: " + r.dump());
        MIB_EXPECT(!facade.fetchMonitoringCellImage(run2, e4, kBase + 20, 0, false, frame), "no image is invented");
        r = pin(facade, run2, e4, kBase + 77);
        MIB_EXPECT(outcome(r) == 4 && r["cell"].is_null(), "unknown row: NotFound " + r.dump());

        // ---- 5. monitoring_clear, unpin, clear --------------------------------------------------
        wd.mark("clear");
        MIB_REQUIRE(pin(facade, run2, e4, kBase + 100000 + 1000)["ok"] == true, "pin a held row");
        MIB_EXPECT(!cells(facade)["strip"].empty(), "a strip before the clear");
        bridge::MonitoringCommand clear;
        clear.action = bridge::MonitoringCommandAction::Clear;
        MIB_REQUIRE(facade.dispatch(clear).ok, "monitoring_clear");
        l = cells(facade);
        MIB_EXPECT(l["strip"].empty() && l["pins"].size() == 2, "the strip clears, the pins stay: " + l.dump());
        auto u = facade.unpinMonitoringCell(run2, e4 + 1, kBase + 20, 0);
        MIB_EXPECT(!u.ok, "unpin names the capture too: " + u.message);
        u = facade.unpinMonitoringCell(run2, e4, kBase + 20, 0);
        MIB_EXPECT(u.ok && u.message == "unpinned", u.message);
        u = facade.unpinMonitoringCell(run2, e4, kBase + 20, 0);
        MIB_EXPECT(!u.ok && u.message.find("not pinned") != std::string::npos, u.message);
        u = facade.clearMonitoringCellPins();
        MIB_EXPECT(u.ok && u.message == "cleared 1 pin(s)", u.message);
        MIB_EXPECT(cells(facade)["pins"].empty(), "cleared");

        bridge::ExperimentCommand stop2;
        stop2.action = bridge::ExperimentCommandAction::Stop;
        MIB_REQUIRE(facade.dispatch(stop2).ok, "stop run 2");
        MIB_REQUIRE(waitFor([&] { return backendApp.experiment().status().state == app::ExperimentRunState::Idle; },
                            std::chrono::seconds(20)),
                    "run 2 finalizes");

        // ---- 6. capture changes, at the moment they happen ---------------------------------------
        wd.mark("roi change");
        MIB_REQUIRE(facade.dispatch(clear).ok, "monitoring_clear");
        auto before = holdTwo(facade, proc, kBase + 300);
        proc.setRealtimeRoi({0, 0, 256, 96}); // realtime still runs on the mock camera
        expectEnded(facade, proc, kBase + 300, before, "the processing ROI changed", "ROI change");
        before = proc.monitoringCaptureEpoch();
        proc.setRealtimeRoi({0, 0, 256, 96});
        MIB_EXPECT(proc.monitoringCaptureEpoch() == before, "the same ROI again is no change");

        wd.mark("realtime stop");
        MIB_REQUIRE(backendApp.capture().isRunning() && proc.isRealtimeRunning(), "the camera and realtime run");
        before = holdTwo(facade, proc, kBase + 305);
        proc.stopRealtime(); // ends the capture; the camera keeps running
        MIB_EXPECT(backendApp.capture().isRunning() && !proc.isRealtimeRunning(), "only realtime stopped");
        expectEnded(facade, proc, kBase + 305, before, "realtime processing stopped", "realtime stop");

        wd.mark("camera stop");
        before = holdTwo(facade, proc, kBase + 310);
        bridge::CameraCommand stopCapture;
        stopCapture.action = bridge::CameraCommandAction::StopCapture;
        MIB_REQUIRE(facade.dispatch(stopCapture).ok, "capture stop");
        expectEnded(facade, proc, kBase + 310, before, "the camera stopped", "camera stop");

        wd.mark("buffer resize");
        before = holdTwo(facade, proc, kBase + 315);
        const auto resized =
            json::parse(facade.savePreviewBufferJson(R"({"action":"resize","capacity":"64","confirm_clear":true})"));
        MIB_REQUIRE(resized["ok"] == true, "resize: " + resized.dump());
        expectEnded(facade, proc, kBase + 315, before, "the frame buffer was resized", "buffer resize");

        wd.mark("overview switch");
        MIB_EXPECT(!proc.isRealtimeRunning(), "realtime stopped: no frame is processed after the switch");
        {
            // A MindVision profile, selected as the Overview test does (SDK-free builds included).
            const auto profile = td.path() / "mindvision.json";
            std::ofstream(profile) << json{{"width", 512}, {"height", 96}, {"offset_x", 0}, {"offset_y", 0}}.dump();
            std::string error;
            MIB_REQUIRE(backendApp.stageMindVisionConfigFromFile(profile.string(), &error), error);
            backendApp.setMindVisionCameraSelection(0, "Test camera");
            MIB_REQUIRE(backendApp.stageMindVisionConfigFromFile(profile.string(), &error), error);
            proc.setRealtimeRoi({0, 0, 512, 96}); // the profile's: the switch below changes no ROI
            before = holdTwo(facade, proc, kBase + 320);
            const auto storeBefore = backendApp.getFrameStore();
            MIB_REQUIRE(backendApp.setMindVisionOverview(true, &error), error);
            MIB_EXPECT(backendApp.getFrameStore() != storeBefore && !backendApp.capture().isRunning() &&
                           !proc.isRealtimeRunning(),
                       "a new frame buffer, nothing running");
            expectEnded(facade, proc, kBase + 320, before, "switched to Overview", "Overview switch");
        }
        facade.shutdown();
    }
    std::printf("monitoring_cells_facade_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
