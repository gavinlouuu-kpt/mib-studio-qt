// monitoring_cell_pipeline_test (#716 A6a review round 1, bridge ABI 41)
//
// Live cell images through the real ProcessingService realtime loop (no fake
// ring), for the two findings of review round 1:
//   1. geometry: with an ROI whose origin is not (0, 0), every strip cell's
//      crop holds its own object's pixels, in both realtime modes. Inline rows
//      are measured on the ROI image; async-batch rows are measured on the full
//      frame and keep an ROI crop, so their geometry must be translated. Each
//      object carries a unique grey level (its marker); the test checks
//      pixels, not sizes: all of the object's marker pixels are in the crop,
//      none of the other object's, and the box centre is the marker.
//   2. capture identity: capture A, then a new FrameStore (as a camera mode
//      switch does) and capture B with the same frame indices and object ids,
//      then a resize of B's store (indices renumbered, capture C). A pin of the
//      new capture's identity gets the new capture's pixels, also while the
//      strip still holds the old capture's entry for that frame; a pin naming
//      an earlier capture is CaptureMismatch; the strip keeps only the current
//      capture's cells; an earlier pin keeps its own pixels under its identity.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/MonitoringCellStore.h"

#include "support/assert.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using backend::playback::FrameStore;
using backend::services::MonitoringCellInfo;
using backend::services::MonitoringCellKey;
using backend::services::MonitoringCellStore;
using backend::services::ProcessedFrame;
using backend::services::ProcessingService;
using Outcome = MonitoringCellStore::PinOutcome;
using Mode = ProcessingService::RealtimeProcessingMode;

namespace {

constexpr int kW = 320;
constexpr int kH = 96;
constexpr uint64_t kMono8 = 0x01080001ULL;
// The ROI: origin (40, 12), so full-frame and ROI coordinates differ.
constexpr int kRoiX = 40, kRoiY = 12, kRoiW = 220, kRoiH = 72;
// Two objects per frame, full-frame coordinates, each with its own marker.
constexpr int kObjW = 16, kObjH = 12;
const cv::Rect kObjA(70, 36, kObjW, kObjH);
const cv::Rect kObjB(190, 44, kObjW, kObjH);
constexpr int kMarkerA = 120, kMarkerB = 220;

backend::services::ProcessingConfig lenient()
{
    backend::services::ProcessingConfig c;
    c.gaussian_blur_size = 3;
    c.bg_subtract_threshold = 8;
    c.morph_kernel_size = 3;
    c.morph_iterations = 1;
    c.enable_border_check = false;
    c.enable_area_range_check = false;
    c.enable_deformability_range_check = false;
    c.enable_area_ratio_check = false;
    c.enable_ring_ratio_check = false;
    c.require_single_inner_contour = false;
    c.empty_frame_pixel_threshold = 1;
    c.auto_background_enabled = false;
    c.multi_image_enabled = false;
    return c;
}

cv::Mat twoObjects(int markerA, int markerB)
{
    cv::Mat m(kH, kW, CV_8UC1, cv::Scalar(0));
    m(kObjA).setTo(markerA);
    m(kObjB).setTo(markerB);
    return m;
}

cv::Mat oneObject(int marker)
{
    cv::Mat m(kH, kW, CV_8UC1, cv::Scalar(0));
    m(kObjA).setTo(marker);
    return m;
}

void push(FrameStore& store, const cv::Mat& m, uint64_t ts)
{
    store.pushFrame(m.data, static_cast<size_t>(m.total()), kW, kH, static_cast<size_t>(kW), kMono8, ts);
}

size_t validHeld(const ProcessingService& proc) { return proc.visitMonitoringNewest(0, false, {}).validHeld; }

// Push frames until the ring holds `rows` valid rows appended since `before` (or time out).
bool feed(ProcessingService& proc, FrameStore& store, const cv::Mat& frame, size_t rows, uint64_t& ts)
{
    const auto before = proc.visitMonitoringNewest(0, false, {}).validAppended;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        push(store, frame, ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        const auto v = proc.visitMonitoringNewest(0, false, {});
        if (v.validAppended - before >= rows) return true;
    }
    return false;
}

std::vector<std::pair<MonitoringCellKey, ProcessingService::MonitoringRowTag>> ringRows(const ProcessingService& proc)
{
    std::vector<std::pair<MonitoringCellKey, ProcessingService::MonitoringRowTag>> out;
    proc.visitMonitoringNewest(2000, true, [&](const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& t, bool) {
        out.push_back({{t.runGeneration, f.index, f.validation.objectId, t.captureEpoch}, t});
    });
    return out;
}

void startProcessing(ProcessingService& proc, Mode mode, const std::shared_ptr<FrameStore>& store, bool roi)
{
    proc.setProcessingConfig(lenient());
    proc.setRealtimeProcessingMode(mode);
    proc.setPixelToMicronFactor(0.5);
    proc.setRealtimeDropFrames(true);
    if (roi) proc.setRealtimeRoi({kRoiX, kRoiY, kRoiW, kRoiH});
    proc.setMonitoringActive(true);
    proc.setRealtimeEnabled(true);
    proc.startRealtime(store);
}

// ---- 1. geometry: crops hold their own object, ROI origin != (0, 0) ----------------------
void geometry(Mode mode, const char* name)
{
    std::printf("geometry, %s mode, ROI at (%d, %d)\n", name, kRoiX, kRoiY);
    auto store = std::make_shared<FrameStore>(256);
    ProcessingService proc;
    proc.start(1);
    startProcessing(proc, mode, store, true);
    uint64_t ts = 0;
    const bool fed = feed(proc, *store, twoObjects(kMarkerA, kMarkerB), 8, ts);
    proc.stopRealtime();
    proc.stop();
    MIB_REQUIRE(fed, std::string("the realtime loop appended valid rows (") + name + ")");

    auto source = backend::services::makeProcessingCellSource(proc);
    MonitoringCellStore cells(*source);
    cells.refreshStrip();
    const auto l = cells.listing();
    MIB_REQUIRE(l.strip.size() >= 2, std::string("strip cells: ") + std::to_string(l.strip.size()));
    std::set<int> seen;
    for (const auto& c : l.strip) {
        MIB_REQUIRE(c.hasImage, "a crop: " + c.noImageReason);
        MIB_EXPECT(c.sourceWidth == kRoiW && c.sourceHeight == kRoiH, "the row's image is the ROI");
        // The box is in ROI coordinates: tell the objects apart by where it is.
        const bool isA = c.metrics.bboxX < (kObjA.x + kObjB.x) / 2.0 - kRoiX;
        const cv::Rect own = (isA ? kObjA : kObjB) - cv::Point(kRoiX, kRoiY);
        const int marker = isA ? kMarkerA : kMarkerB, other = isA ? kMarkerB : kMarkerA;
        MIB_EXPECT(std::abs(c.metrics.bboxX - own.x) <= 2 && std::abs(c.metrics.bboxY - own.y) <= 2,
                   std::string(name) + ": box in ROI coordinates, at " + std::to_string(c.metrics.bboxX) + "," +
                       std::to_string(c.metrics.bboxY) + " for " + std::to_string(own.x) + "," +
                       std::to_string(own.y));
        cv::Mat crop;
        MIB_REQUIRE(cells.image(c.key, false, crop), "image");
        MIB_EXPECT(cv::countNonZero(crop == marker) == kObjW * kObjH,
                   std::string(name) + ": the crop holds all of its object's pixels (" +
                       std::to_string(cv::countNonZero(crop == marker)) + " of " + std::to_string(kObjW * kObjH) + ")");
        MIB_EXPECT(cv::countNonZero(crop == other) == 0, std::string(name) + ": none of the other object's");
        const int cx = static_cast<int>(c.metrics.bboxX + c.metrics.bboxWidth / 2) - c.cropX;
        const int cy = static_cast<int>(c.metrics.bboxY + c.metrics.bboxHeight / 2) - c.cropY;
        MIB_EXPECT(cx >= 0 && cy >= 0 && cx < crop.cols && cy < crop.rows && crop.at<std::uint8_t>(cy, cx) == marker,
                   std::string(name) + ": the box centre is the object's marker");
        seen.insert(marker);
    }
    MIB_EXPECT(seen.size() == 2, std::string(name) + ": both objects reached the strip");
    // A pin of a ring row (not via the strip) holds the same object.
    const auto rows = ringRows(proc);
    MIB_REQUIRE(!rows.empty(), "ring rows");
    const auto key = rows.front().first; // the oldest valid row: not in the strip
    MIB_EXPECT(mode == Mode::Inline ? (rows.front().second.geometryOriginX == 0)
                                    : (rows.front().second.geometryOriginX == kRoiX &&
                                       rows.front().second.geometryOriginY == kRoiY),
               std::string(name) + ": the tag records where the geometry's origin is");
    const auto r = cells.pin(key);
    MIB_REQUIRE(r.outcome == Outcome::Pinned && r.cell, r.message);
    cv::Mat crop;
    MIB_REQUIRE(cells.image(key, false, crop), "pin image");
    const int mA = cv::countNonZero(crop == kMarkerA), mB = cv::countNonZero(crop == kMarkerB);
    MIB_EXPECT((mA == kObjW * kObjH && mB == 0) || (mB == kObjW * kObjH && mA == 0),
               std::string(name) + ": the pinned crop holds exactly its own object");
}

// ---- 2. capture identity across a FrameStore swap and a resize ----------------------------
uint64_t newestFrame(const MonitoringCellStore::Listing& l)
{
    uint64_t f = 0;
    for (const auto& c : l.strip) f = std::max(f, c.key.frameIndex);
    return f;
}

bool holds(const std::vector<std::pair<MonitoringCellKey, ProcessingService::MonitoringRowTag>>& rows,
           const MonitoringCellKey& k)
{
    return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == k; });
}

void captureIdentity()
{
    std::printf("capture identity: store swap, then resize\n");
    constexpr int kA = 90, kB = 200, kC = 150;
    ProcessingService proc;
    proc.start(1);
    auto source = backend::services::makeProcessingCellSource(proc);
    MonitoringCellStore cells(*source);

    // Capture A.
    auto storeA = std::make_shared<FrameStore>(256);
    startProcessing(proc, Mode::Inline, storeA, false);
    uint64_t ts = 0;
    MIB_REQUIRE(feed(proc, *storeA, oneObject(kA), 12, ts), "capture A rows");
    proc.stopRealtime();
    const auto eA = proc.monitoringCaptureEpoch();
    cells.refreshStrip();
    auto l = cells.listing();
    MIB_REQUIRE(!l.strip.empty() && l.ringCaptureEpoch == eA, "A's strip");
    const uint64_t fA = newestFrame(l); // in A's strip
    const int obj = l.strip.back().key.objectId; // one object per frame: the same id every frame
    const MonitoringCellKey pinA = l.strip.front().key;
    MIB_REQUIRE(pinA.captureEpoch == eA && pinA.objectId == obj, "A's identity");
    MIB_REQUIRE(cells.pin(pinA).outcome == Outcome::Pinned, "pin A's oldest strip cell");

    // Capture B: a new FrameStore (a camera mode switch), indices restart from the start.
    auto storeB = std::make_shared<FrameStore>(256);
    proc.startRealtime(storeB);
    const auto eB = proc.monitoringCaptureEpoch();
    MIB_EXPECT(eB > eA, "a new capture epoch");
    const auto bDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!holds(ringRows(proc), {0, fA, obj, eB}) && std::chrono::steady_clock::now() < bDeadline) {
        push(*storeB, oneObject(kB), ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    proc.stopRealtime();
    auto rows = ringRows(proc);
    MIB_REQUIRE(holds(rows, {0, fA, obj, eB}) && holds(rows, {0, fA, obj, eA}),
                "the ring holds frame " + std::to_string(fA) + " object " + std::to_string(obj) + " of both captures");

    // No refresh yet: the strip still holds A's frame fA. B's identity must get B's pixels.
    auto r = cells.pin({0, fA, obj, eB});
    MIB_REQUIRE(r.outcome == Outcome::Pinned, "B's frame pins: " + r.message);
    cv::Mat px;
    MIB_REQUIRE(cells.image({0, fA, obj, eB}, false, px), "B's pin image");
    MIB_EXPECT(cv::countNonZero(px == kB) == kObjW * kObjH && cv::countNonZero(px == kA) == 0,
               "B's pixels, never A's (the stale strip entry)");
    // A's identity now: refused, even though the ring still holds A's row.
    r = cells.pin({0, fA, obj, eA});
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch && !r.cell, "A's identity is CaptureMismatch: " + r.message);
    // The strip moves to B.
    cells.refreshStrip();
    l = cells.listing();
    MIB_EXPECT(l.ringCaptureEpoch == eB && !l.strip.empty(), "B's strip");
    for (const auto& c : l.strip) {
        MIB_EXPECT(c.key.captureEpoch == eB, "only B's cells");
        MIB_EXPECT(cells.image(c.key, false, px) && cv::countNonZero(px == kB) == kObjW * kObjH, "B's pixels");
    }
    // A's earlier pin keeps A's pixels under A's identity.
    MIB_EXPECT(cells.image(pinA, false, px) && cv::countNonZero(px == kA) == kObjW * kObjH &&
                   cv::countNonZero(px == kB) == 0,
               "A's pin is A's");

    // Capture C: B's store renumbers its frames (a resize below what it holds clears it and
    // restarts the indices at 0) while the realtime loop keeps running.
    proc.startRealtime(storeB);
    const auto eB2 = proc.monitoringCaptureEpoch();
    MIB_REQUIRE(feed(proc, *storeB, oneObject(kB), 6, ts), "B rows before the resize");
    MonitoringCellKey bKey;
    for (const auto& row : ringRows(proc))
        if (row.first.captureEpoch == eB2) bKey = row.first;
    MIB_REQUIRE(bKey.captureEpoch == eB2, "a B row of the running capture");
    MIB_REQUIRE(storeB->resize(8), "resize");
    MIB_REQUIRE(feed(proc, *storeB, oneObject(kC), 6, ts), "capture C rows");
    proc.stopRealtime();
    proc.stop();
    const auto eC = proc.monitoringCaptureEpoch();
    MIB_EXPECT(eC > eB2, "a renumbering store is a new capture epoch, without a realtime restart");
    r = cells.pin({0, fA, obj, eB});
    MIB_EXPECT(r.outcome == Outcome::AlreadyPinned, "the existing B pin is found by its identity: " + r.message);
    r = cells.pin(bKey);
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch, "B's identity after the resize: " + r.message);
    cells.refreshStrip();
    l = cells.listing();
    MIB_EXPECT(l.ringCaptureEpoch == eC && !l.strip.empty(), "C's strip");
    for (const auto& c : l.strip)
        MIB_EXPECT(c.key.captureEpoch == eC && cells.image(c.key, false, px) &&
                       cv::countNonZero(px == kC) == kObjW * kObjH && cv::countNonZero(px == kB) == 0,
                   "only C's cells, with C's pixels");
}

} // namespace

int main()
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(150);
    wd.mark("geometry inline");
    geometry(Mode::Inline, "inline");
    wd.mark("geometry async batch");
    geometry(Mode::AsyncBatch, "async batch");
    wd.mark("capture identity");
    captureIdentity();
    std::printf("monitoring_cell_pipeline_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
