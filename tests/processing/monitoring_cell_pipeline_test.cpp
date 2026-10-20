// monitoring_cell_pipeline_test (#716 A6a review rounds 1-3, bridge ABI 41)
//
// Live cell images through the real ProcessingService realtime loop (no fake
// ring), with the live cell store wired as AppBackend wires it (the one
// capture authority), for the findings of review rounds 1 to 3:
//   1. geometry: with an ROI whose origin is not (0, 0), every strip cell's
//      crop holds its own object's pixels, in both realtime modes. Inline rows
//      are measured on the ROI image; async-batch rows are measured on the full
//      frame and keep an ROI crop, so their geometry must be translated. Each
//      object carries a unique grey level (its marker); the test checks
//      pixels, not sizes: all of the object's marker pixels are in the crop,
//      none of the other object's, and the box centre is the marker.
//   2. capture identity: capture A, then a realtime restart on a replacement
//      FrameStore (as a camera mode switch does) and capture B with the same
//      frame indices and object ids, then a restart on B's store and a resize
//      of it (indices renumbered). Pins belong to the capture that made them:
//      at the restart, before any frame of B and before any refresh, A's
//      strip image and A's pinned image (and mask) are refused with
//      CaptureMismatch and A's pin keeps its measurements only; a pin of B's
//      identity gets B's pixels; after the resize, the renumbered frames are
//      never cells until the change is made (the facade's resize command
//      makes it), and then only with C's pixels.
//   3. tag at entry (review round 3): a frame held between its entry and its
//      completion (the append hook) while the ROI changes, then released, in
//      both realtime modes: its rows carry the capture it entered with, it
//      never enters the strip and its identity is refused.
//   4. stopping realtime ends the capture while the camera keeps producing
//      frames: the strip is cleared, the old identities are refused, the pin
//      keeps its measurements only ("capture E ended").
//   5. the camera session floor: frames of a camera session that closed,
//      still arriving or still in processing after the change, enter with no
//      capture and never become cells of the next one; the next session's do.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/MonitoringCellStore.h"

#include "support/assert.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
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

void push(FrameStore& store, const cv::Mat& m, uint64_t ts, uint64_t captureSession = 0)
{
    store.pushFrame(m.data, static_cast<size_t>(m.total()), kW, kH, static_cast<size_t>(kW), kMono8, ts, 0,
                    captureSession);
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

// The live cell store wired to a service as AppBackend wires it: the one capture authority.
struct Wired {
    ProcessingService& proc;
    std::unique_ptr<backend::services::MonitoringCellSource> source;
    std::unique_ptr<MonitoringCellStore> cells;
    explicit Wired(ProcessingService& p)
        : proc(p), source(backend::services::makeProcessingCellSource(p)),
          cells(std::make_unique<MonitoringCellStore>(*source))
    {
        proc.setMonitoringCaptureAuthority([c = cells.get()](const char* reason) { return c->advanceCapture(reason); });
    }
    ~Wired()
    {
        proc.stopRealtime();
        proc.setMonitoringCaptureAuthority({});
    }
};

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
    Wired wired(proc);
    auto& cells = *wired.cells;
    startProcessing(proc, mode, store, true);
    uint64_t ts = 0;
    const bool fed = feed(proc, *store, twoObjects(kMarkerA, kMarkerB), 8, ts);
    MIB_REQUIRE(fed, std::string("the realtime loop appended valid rows (") + name + ")");
    // Realtime keeps running (stopping it ends the capture); no more frames arrive.
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
    Wired wired(proc);
    auto& cells = *wired.cells;

    // Capture A.
    auto storeA = std::make_shared<FrameStore>(256);
    startProcessing(proc, Mode::Inline, storeA, false);
    const auto eA = proc.monitoringCaptureEpoch();
    MIB_EXPECT(eA == cells.currentCapture(), "the store published the epoch the loop tags with");
    uint64_t ts = 0;
    MIB_REQUIRE(feed(proc, *storeA, oneObject(kA), 12, ts), "capture A rows");
    cells.refreshStrip();
    auto l = cells.listing();
    MIB_REQUIRE(!l.strip.empty() && l.ringCaptureEpoch == eA, "A's strip");
    const uint64_t fA = newestFrame(l); // in A's strip
    const int obj = l.strip.back().key.objectId; // one object per frame: the same id every frame
    const MonitoringCellKey pinA = l.strip.front().key;
    const MonitoringCellKey stripA = l.strip.back().key;
    MIB_REQUIRE(pinA.captureEpoch == eA && pinA.objectId == obj && stripA != pinA, "A's identities");
    MIB_REQUIRE(cells.pin(pinA).outcome == Outcome::Pinned, "pin A's oldest strip cell");
    cv::Mat px;
    MIB_REQUIRE(cells.image(stripA, false, px) && cells.image(pinA, false, px), "A's images are served");

    // Capture B: a restart on a replacement FrameStore (a camera mode switch), indices restart.
    proc.stopRealtime(); // ends A
    auto storeB = std::make_shared<FrameStore>(256);
    proc.startRealtime(storeB);
    const auto eB = proc.monitoringCaptureEpoch();
    MIB_EXPECT(eB > eA && eB == cells.currentCapture(), "a new capture epoch, published by the store");
    // At once: no frame of B yet, no refresh. A's strip image and A's pinned image are refused.
    px.release();
    auto read = cells.image(stripA, false, px);
    MIB_EXPECT(!read && read.captureMismatch && px.empty(), "A's strip image is refused: " + read.message);
    read = cells.image(pinA, false, px);
    MIB_EXPECT(!read && read.captureMismatch && px.empty(), "A's pinned image is refused: " + read.message);
    read = cells.image(pinA, true, px);
    MIB_EXPECT(!read && read.captureMismatch && px.empty(), "A's pinned mask is refused: " + read.message);
    l = cells.listing();
    MIB_EXPECT(l.strip.empty() && l.stats.stripCells == 0 && l.stats.pinBytes == 0 && l.stats.stripBytes == 0 &&
                   l.pins.size() == 1 && l.pins[0].key == pinA && !l.pins[0].hasImage,
               "A's copies were released; A's pin keeps its measurements");
    MIB_EXPECT(l.lastCaptureChange && l.lastCaptureChange->captureEpoch == eB &&
                   l.lastCaptureChange->reason == "realtime processing started",
               "the listing says why");
    MIB_EXPECT(cells.pin(pinA).outcome == Outcome::CaptureMismatch, "A's pin identity is CaptureMismatch");
    const auto bDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!holds(ringRows(proc), {0, fA, obj, eB}) && std::chrono::steady_clock::now() < bDeadline) {
        push(*storeB, oneObject(kB), ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    auto rows = ringRows(proc);
    MIB_REQUIRE(holds(rows, {0, fA, obj, eB}) && holds(rows, {0, fA, obj, eA}),
                "the ring holds frame " + std::to_string(fA) + " object " + std::to_string(obj) + " of both captures");

    // B's identity of frame fA gets B's pixels.
    auto r = cells.pin({0, fA, obj, eB});
    MIB_REQUIRE(r.outcome == Outcome::Pinned, "B's frame pins: " + r.message);
    MIB_REQUIRE(cells.image({0, fA, obj, eB}, false, px), "B's pin image");
    MIB_EXPECT(cv::countNonZero(px == kB) == kObjW * kObjH && cv::countNonZero(px == kA) == 0,
               "B's pixels, never A's");
    // A's identity now: refused, even though the ring still holds A's row.
    r = cells.pin({0, fA, obj, eA});
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch && !r.cell, "A's identity is CaptureMismatch: " + r.message);
    // The strip is B's.
    cells.refreshStrip();
    l = cells.listing();
    MIB_EXPECT(l.ringCaptureEpoch == eB && !l.strip.empty(), "B's strip");
    for (const auto& c : l.strip) {
        MIB_EXPECT(c.key.captureEpoch == eB, "only B's cells");
        MIB_EXPECT(cells.image(c.key, false, px) && cv::countNonZero(px == kB) == kObjW * kObjH, "B's pixels");
    }
    MIB_EXPECT(!cells.image(pinA, false, px), "A's pin stays refused");

    // A restart on B's store is a new capture too: B's pin is ended.
    proc.stopRealtime();
    proc.startRealtime(storeB);
    const auto eB2 = proc.monitoringCaptureEpoch();
    l = cells.listing();
    bool bEnded = false;
    for (const auto& p : l.pins) bEnded = bEnded || (p.key.captureEpoch == eB && !p.hasImage);
    MIB_EXPECT(eB2 > eB && bEnded, "the restart ended B's pin");
    MIB_REQUIRE(feed(proc, *storeB, oneObject(kB), 6, ts), "B rows before the resize");
    cells.refreshStrip();
    l = cells.listing();
    MIB_REQUIRE(!l.strip.empty() && l.strip.back().key.captureEpoch == eB2, "B's strip of the running capture");
    const MonitoringCellKey bKey = l.strip.back().key;
    MIB_REQUIRE(cells.pin(bKey).outcome == Outcome::Pinned, "pin B");
    // B's store renumbers its frames (a resize below what it holds clears it and restarts the
    // indices at 0) while the realtime loop keeps running. Nothing tells the store yet (the
    // facade's resize command does, below): the renumbered frames are of another store
    // generation than the one the epoch was bound to, so they enter with no capture and are
    // never cells; every cell still served is one of B's, by B's identity.
    MIB_REQUIRE(storeB->resize(8), "resize");
    const auto appendedBefore = proc.visitMonitoringNewest(0, false, {}).validAppended;
    MIB_REQUIRE(feed(proc, *storeB, oneObject(kC), 6, ts), "renumbered rows");
    MIB_EXPECT(proc.monitoringCaptureEpoch() == eB2, "no capture change without the call");
    size_t renumbered = 0;
    proc.visitMonitoringNewest(2000, true, [&](const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& t, bool) {
        if (f.originalImage.empty()) return;
        if (cv::countNonZero(f.originalImage == kC) > 0) {
            ++renumbered;
            MIB_EXPECT(t.captureEpoch == 0, "a renumbered frame enters with no capture (frame " +
                                                std::to_string(f.index) + ", epoch " + std::to_string(t.captureEpoch) + ")");
        }
    });
    MIB_EXPECT(renumbered > 0 && proc.visitMonitoringNewest(0, false, {}).validAppended > appendedBefore,
               "the renumbered frames are monitoring rows");
    cells.refreshStrip();
    l = cells.listing();
    for (const auto& c : l.strip)
        MIB_EXPECT(c.key.captureEpoch == eB2 && cells.image(c.key, false, px) && cv::countNonZero(px == kC) == 0,
                   "no renumbered frame is a cell");
    MIB_EXPECT(cells.image(bKey, false, px) && cv::countNonZero(px == kB) == kObjW * kObjH, "B's pin is B's");
    // Capture C: the facade's resize command makes the change.
    const auto eC = cells.advanceCapture("the frame buffer was resized (its frames were renumbered)");
    MIB_EXPECT(eC > eB2 && proc.monitoringCaptureEpoch() == eC, "published");
    read = cells.image(bKey, false, px);
    MIB_EXPECT(!read && read.captureMismatch, "B's pinned image after the change is refused: " + read.message);
    r = cells.pin(bKey);
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch, "B's identity after the change: " + r.message);
    MIB_REQUIRE(feed(proc, *storeB, oneObject(kC), 6, ts), "capture C rows");
    cells.refreshStrip();
    l = cells.listing();
    MIB_EXPECT(l.ringCaptureEpoch == eC && !l.strip.empty(), "C's strip");
    for (const auto& c : l.strip)
        MIB_EXPECT(c.key.captureEpoch == eC && cells.image(c.key, false, px) &&
                       cv::countNonZero(px == kC) == kObjW * kObjH && cv::countNonZero(px == kB) == 0,
                   "only C's cells, with C's pixels");
}

// ---- 3. tag at entry: a frame held between its entry and its completion --------------------
void heldAcrossAnRoiChange(Mode mode, const char* name)
{
    std::printf("a frame held mid-processing while the ROI changes, %s mode\n", name);
    auto store = std::make_shared<FrameStore>(256);
    ProcessingService proc;
    proc.start(1);
    Wired wired(proc);
    auto& cells = *wired.cells;
    startProcessing(proc, mode, store, true); // ROI A: (kRoiX, kRoiY, kRoiW, kRoiH)
    uint64_t ts = 0;
    MIB_REQUIRE(feed(proc, *store, twoObjects(kMarkerA, kMarkerB), 4, ts), "rows of ROI A");
    const auto eA = proc.monitoringCaptureEpoch();

    // Hold the next frame whose rows are about to be appended.
    std::mutex m;
    std::condition_variable wake;
    bool holding = false, release = false;
    uint64_t held = 0;
    std::atomic<bool> armed{true};
    proc.setMonitoringAppendHookForTests([&](uint64_t frameIndex) {
        if (!armed.exchange(false)) return;
        std::unique_lock lk(m);
        held = frameIndex;
        holding = true;
        wake.notify_all();
        wake.wait_for(lk, std::chrono::seconds(20), [&] { return release; });
    });
    {
        std::unique_lock lk(m);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!holding && std::chrono::steady_clock::now() < deadline) {
            lk.unlock();
            push(*store, twoObjects(kMarkerA, kMarkerB), ++ts);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            lk.lock();
        }
        MIB_REQUIRE(holding, std::string(name) + ": a frame is held before its rows are appended");
    }
    // The ROI changes while the frame is held: a new capture, at once.
    proc.setRealtimeRoi({kRoiX + 10, kRoiY, kRoiW - 10, kRoiH});
    const auto eB = proc.monitoringCaptureEpoch();
    MIB_REQUIRE(eB > eA && cells.currentCapture() == eB, "the ROI change is a capture change");
    proc.setMonitoringAppendHookForTests({});
    {
        std::scoped_lock lk(m);
        release = true;
    }
    wake.notify_all();
    // The held frame's rows reach the ring.
    std::vector<std::pair<MonitoringCellKey, ProcessingService::MonitoringRowTag>> heldRows;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (heldRows.empty() && std::chrono::steady_clock::now() < deadline) {
        for (const auto& r : ringRows(proc))
            if (r.first.frameIndex == held) heldRows.push_back(r);
        if (heldRows.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    MIB_REQUIRE(!heldRows.empty(), std::string(name) + ": the held frame's rows were appended");
    for (const auto& r : heldRows)
        MIB_EXPECT(r.first.captureEpoch == eA, std::string(name) + ": the held frame carries the capture it entered with (" +
                                                   std::to_string(r.first.captureEpoch) + ", entered in " +
                                                   std::to_string(eA) + ", now " + std::to_string(eB) + ")");
    // Frames of ROI B become cells; the held frame never does.
    MonitoringCellStore::Listing l;
    const auto bDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < bDeadline) {
        push(*store, twoObjects(kMarkerA, kMarkerB), ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        cells.refreshStrip();
        l = cells.listing();
        if (l.strip.size() >= 2) break;
    }
    MIB_EXPECT(l.strip.size() >= 2 && l.ringCaptureEpoch == eB, std::string(name) + ": ROI B's strip");
    for (const auto& c : l.strip) {
        MIB_EXPECT(c.key.frameIndex != held, std::string(name) + ": the held frame never enters the strip");
        MIB_EXPECT(c.key.captureEpoch == eB && c.sourceWidth == kRoiW - 10, std::string(name) + ": cells of ROI B");
    }
    for (const auto& r : heldRows) {
        const MonitoringCellKey asEntered = r.first;
        MonitoringCellKey asNow = r.first;
        asNow.captureEpoch = eB;
        auto p = cells.pin(asEntered);
        MIB_EXPECT(p.outcome == Outcome::CaptureMismatch, std::string(name) + ": its key is refused: " + p.message);
        p = cells.pin(asNow);
        MIB_EXPECT(p.outcome == Outcome::NotFound, std::string(name) + ": it is no frame of ROI B: " + p.message);
        cv::Mat px;
        const auto read = cells.image(asEntered, false, px);
        MIB_EXPECT(!read && read.captureMismatch && px.empty(), std::string(name) + ": no image: " + read.message);
    }
}

// ---- 4. stopping realtime ends the capture (the camera keeps producing) --------------------
void stopEndsTheCapture()
{
    std::printf("stopping realtime ends the capture while frames keep arriving\n");
    auto store = std::make_shared<FrameStore>(256);
    ProcessingService proc;
    proc.start(1);
    Wired wired(proc);
    auto& cells = *wired.cells;
    startProcessing(proc, Mode::Inline, store, false);
    std::atomic<bool> producing{true};
    std::atomic<uint64_t> ts{0};
    std::thread camera([&] { // the camera keeps running throughout
        while (producing) {
            push(*store, oneObject(170), ++ts);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    MonitoringCellStore::Listing l;
    while (std::chrono::steady_clock::now() < deadline) {
        cells.refreshStrip();
        l = cells.listing();
        if (l.strip.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    MIB_REQUIRE(l.strip.size() >= 2, "strip cells while realtime runs");
    const auto e = l.ringCaptureEpoch;
    const auto pinned = l.strip.front().key, stripped = l.strip.back().key;
    MIB_REQUIRE(cells.pin(pinned).outcome == Outcome::Pinned, "pin");
    cv::Mat px;
    MIB_REQUIRE(cells.image(pinned, false, px) && cells.image(stripped, false, px), "served while it runs");

    proc.stopRealtime(); // the camera is still producing
    MIB_EXPECT(proc.monitoringCaptureEpoch() > e && cells.currentCapture() > e, "the stop is a capture change");
    for (const auto& k : {pinned, stripped}) {
        for (const bool mask : {false, true}) {
            px.release();
            const auto read = cells.image(k, mask, px);
            MIB_EXPECT(!read && read.captureMismatch && px.empty() &&
                           read.message.find("realtime processing stopped") != std::string::npos,
                       "an old key is refused after the stop: " + read.message);
        }
        MIB_EXPECT(cells.pin(k).outcome == Outcome::CaptureMismatch, "and cannot be pinned");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // frames keep arriving, unprocessed
    cells.refreshStrip();
    l = cells.listing();
    MIB_EXPECT(l.strip.empty() && l.stats.stripBytes == 0, "the strip is cleared");
    MIB_REQUIRE(l.pins.size() == 1, "the pin stays");
    MIB_EXPECT(l.pins[0].key == pinned && !l.pins[0].hasImage && l.stats.pinBytes == 0 &&
                   l.pins[0].noImageReason.find("capture " + std::to_string(e) +
                                                " ended (realtime processing stopped)") != std::string::npos,
               "metadata only, capture ended: " + l.pins[0].noImageReason);
    MIB_EXPECT(l.lastCaptureChange && l.lastCaptureChange->reason == "realtime processing stopped" &&
                   l.lastCaptureChange->pinsEnded == 1,
               "the listing says why");
    producing = false;
    camera.join();
}

// ---- 5. the camera session floor ---------------------------------------------------------
void sessionFloor()
{
    std::printf("frames of a closed camera session never adopt the next capture\n");
    constexpr int kOld = 60, kNew = 240;
    auto store = std::make_shared<FrameStore>(256);
    ProcessingService proc;
    proc.start(1);
    Wired wired(proc);
    auto& cells = *wired.cells;
    startProcessing(proc, Mode::Inline, store, false);
    uint64_t ts = 0;
    const auto feedSession = [&](int marker, uint64_t session, size_t rows) {
        const auto before = proc.visitMonitoringNewest(0, false, {}).validAppended;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            push(*store, oneObject(marker), ++ts, session);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            if (proc.visitMonitoringNewest(0, false, {}).validAppended - before >= rows) return true;
        }
        return false;
    };
    MIB_REQUIRE(feedSession(kOld, 5, 4), "session 5 rows");
    // The camera closes session 5 (AppBackend: floor = session + 1).
    const auto e = cells.advanceCapture("the camera stopped", 6);
    MIB_REQUIRE(proc.monitoringCaptureEpoch() == e, "published");
    // Late frames of session 5 (the camera's last frames, a slow consumer).
    MIB_REQUIRE(feedSession(kOld, 5, 4), "late session 5 rows");
    size_t late = 0;
    proc.visitMonitoringNewest(2000, true, [&](const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& t, bool) {
        if (f.originalImage.empty()) return;
        if (cv::countNonZero(f.originalImage == kOld) > 0 && t.captureEpoch >= e) ++late;
    });
    MIB_EXPECT(late == 0, "no row of session 5 carries the new capture (" + std::to_string(late) + " did)");
    cells.refreshStrip();
    MIB_EXPECT(cells.listing().strip.empty(), "no session 5 cell after the change");
    // Session 6 opens: its frames are the new capture's cells.
    MIB_REQUIRE(feedSession(kNew, 6, 4), "session 6 rows");
    cells.refreshStrip();
    const auto l = cells.listing();
    MIB_EXPECT(!l.strip.empty(), "session 6 cells");
    cv::Mat px;
    for (const auto& c : l.strip)
        MIB_EXPECT(c.key.captureEpoch == e && cells.image(c.key, false, px) && cv::countNonZero(px == kNew) == kObjW * kObjH,
                   "only session 6's pixels");
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
    wd.mark("held across an ROI change, inline");
    heldAcrossAnRoiChange(Mode::Inline, "inline");
    wd.mark("held across an ROI change, async batch");
    heldAcrossAnRoiChange(Mode::AsyncBatch, "async batch");
    wd.mark("stop ends the capture");
    stopEndsTheCapture();
    wd.mark("session floor");
    sessionFloor();
    std::printf("monitoring_cell_pipeline_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
