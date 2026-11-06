#include "backend/services/MonitoringCellStore.h"

#include "backend/processing/ProcessingService.h"

#include <opencv2/core.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace backend::services {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedUs(Clock::time_point from, Clock::time_point to)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

std::string runName(std::uint64_t generation)
{
    return generation == 0 ? std::string("outside a run") : "in run " + std::to_string(generation);
}

std::string cellName(std::uint64_t frameIndex, int objectId)
{
    return "frame " + std::to_string(frameIndex) + " object " + std::to_string(objectId);
}

// The plain reasons a cell has no image (bridge-contract.json monitoring_cells.no_image_reasons).
constexpr const char* kNoImageResultsOnly =
    "no image: this instrument reports cell measurements without images";
constexpr const char* kNoImageReleased =
    "no image: the monitoring buffer keeps images for its newest rows only (memory budget); "
    "the measurements are kept";
constexpr const char* kNoImageFormat = "no image: the frame's pixel format is not 8- or 16-bit";
// The capture change of a run start (beginRun).
constexpr const char* kRunStarted = "a run started";

// 8-bit single channel view or copy of a monitoring image; empty when unsupported.
cv::Mat toMono8(const cv::Mat& image)
{
    if (image.empty()) return {};
    cv::Mat plane = image;
    if (image.channels() > 1) cv::extractChannel(image, plane, 0); // channel 0, as review images
    if (plane.depth() == CV_8U) return plane;
    if (plane.depth() == CV_16U) {
        cv::Mat out;
        plane.convertTo(out, CV_8U, 1.0 / 256.0);
        return out;
    }
    return {};
}

bool finite(double v) { return std::isfinite(v); }

} // namespace

MonitoringCellMetrics monitoringCellMetrics(const FilterResult& v, std::uint64_t timestampNs, cv::Point geometryOrigin)
{
    MonitoringCellMetrics m;
    m.timestampNs = timestampNs;
    m.valid = v.isValid;
    m.targetGroup = v.isTargetGroup;
    m.objectCount = v.objectCount;
    m.trackId = v.trackId;
    // Image (ROI) coordinates: an async-batch row was measured on the full frame.
    m.centroidX = v.centroidX - geometryOrigin.x;
    m.centroidY = v.centroidY - geometryOrigin.y;
    m.bboxX = v.bboxX - geometryOrigin.x;
    m.bboxY = v.bboxY - geometryOrigin.y;
    m.bboxWidth = v.bboxWidth;
    m.bboxHeight = v.bboxHeight;
    m.area = v.area;
    m.deformability = v.deformability;
    m.areaRatio = v.areaRatio;
    m.ringRatio = v.ringRatio;
    m.youngsModulus = v.youngsModulus;
    m.pixelToMicron = v.analysisPixelToMicronFactor;
    return m;
}

const char* monitoringReasonName(processing::science::InvalidReasonCode code)
{
    using Code = processing::science::InvalidReasonCode;
    switch (code) {
    case Code::NoContour: return "no_contour";
    case Code::Border: return "border";
    case Code::Area: return "area";
    case Code::Ring: return "ring";
    case Code::Deform: return "deform";
    case Code::AreaRatio: return "area_ratio";
    case Code::Laplacian: return "focus";
    case Code::Channel: return "wall";
    }
    return "unknown";
}

std::vector<MonitoringCellReason> monitoringCellReasons(const MonitoringCellMetrics& metrics,
                                                        const MonitoringCellVerdictInputs& verdict,
                                                        const MonitoringReasonContext& context)
{
    namespace science = processing::science;
    std::vector<MonitoringCellReason> out;
    if (metrics.valid) return out;
    // The row as the classifier reads it (every field it reads, nothing else).
    FilterResult r;
    r.isValid = false;
    r.touchesBorder = verdict.touchesBorder;
    r.inChannel = verdict.inChannel;
    r.degenerateContour = verdict.degenerateContour;
    r.innerContourCount = verdict.innerContourCount;
    r.laplacianVariance = verdict.laplacianVariance;
    r.centroidY = verdict.centroidRow;
    r.area = metrics.area;
    r.ringRatio = metrics.ringRatio;
    r.deformability = metrics.deformability;
    r.areaRatio = metrics.areaRatio;
    // The calibration the row was analysed with decided its area gate; the current one only
    // when the row does not carry it.
    double p2m = metrics.pixelToMicron > 0.0 ? metrics.pixelToMicron : context.pixelToMicron;
    if (!(p2m > 0.0)) p2m = 1.0;
    const ProcessingConfig& cfg = context.config;
    for (const auto code : science::classifyInvalidReasons(r, cfg, p2m)) {
        MonitoringCellReason reason;
        reason.code = code;
        using Code = science::InvalidReasonCode;
        switch (code) {
        case Code::NoContour:
        case Code::Border:
            break; // early exits: no metric was computed
        case Code::Area:
            reason.value = r.area * p2m * p2m;
            reason.min = cfg.area_threshold_min;
            reason.max = cfg.area_threshold_max;
            break;
        case Code::Ring:
            reason.value = r.ringRatio;
            reason.min = cfg.ring_ratio_min;
            reason.max = cfg.ring_ratio_max;
            break;
        case Code::Deform:
            reason.value = r.deformability;
            reason.min = cfg.deformability_threshold_min;
            reason.max = cfg.deformability_threshold_max;
            break;
        case Code::AreaRatio:
            reason.value = r.areaRatio;
            reason.max = cfg.area_ratio_threshold_max; // a ceiling only
            break;
        case Code::Laplacian:
            reason.value = r.laplacianVariance;
            reason.min = cfg.laplacian_variance_min;
            reason.max = cfg.laplacian_variance_max;
            break;
        case Code::Channel:
            reason.value = r.centroidY;
            if (cfg.channel_band_h > 0) {
                reason.min = cfg.channel_band_y;
                reason.max = static_cast<double>(cfg.channel_band_y) + cfg.channel_band_h;
            }
            break;
        }
        out.push_back(reason);
    }
    return out;
}

MonitoringCellStore::MonitoringCellStore(MonitoringCellSource& source) : MonitoringCellStore(source, Policy{}) {}

MonitoringCellStore::MonitoringCellStore(MonitoringCellSource& source, Policy policy)
    : source_(source), policy_(policy), current_(source.currentCaptureEpoch())
{
}

cv::Rect MonitoringCellStore::cropRect(int imageWidth, int imageHeight, const MonitoringCellMetrics& m,
                                       const Policy& p)
{
    if (imageWidth <= 0 || imageHeight <= 0) return {};
    const double maxSide = std::max(1, p.cropMaxSide);
    const bool boxKnown = finite(m.bboxX) && finite(m.bboxY) && finite(m.bboxWidth) && finite(m.bboxHeight) &&
                          m.bboxWidth > 0.0 && m.bboxHeight > 0.0 && m.bboxX < imageWidth && m.bboxY < imageHeight &&
                          m.bboxX + m.bboxWidth > 0.0 && m.bboxY + m.bboxHeight > 0.0;
    double cx = imageWidth / 2.0, cy = imageHeight / 2.0, w = maxSide / 2.0, h = maxSide / 2.0;
    if (boxKnown) {
        cx = m.bboxX + m.bboxWidth / 2.0;
        cy = m.bboxY + m.bboxHeight / 2.0;
        const double margin = std::max<double>(p.cropMinMargin, std::max(m.bboxWidth, m.bboxHeight) / 2.0);
        w = m.bboxWidth + 2.0 * margin;
        h = m.bboxHeight + 2.0 * margin;
    } else if (finite(m.centroidX) && finite(m.centroidY) && m.centroidX >= 0.0 && m.centroidY >= 0.0 &&
               m.centroidX < imageWidth && m.centroidY < imageHeight &&
               !(m.centroidX == 0.0 && m.centroidY == 0.0)) { // exactly (0, 0): nothing was measured
        cx = m.centroidX;
        cy = m.centroidY;
    }
    const int iw = static_cast<int>(std::min<double>(std::ceil(std::min(w, maxSide)), imageWidth));
    const int ih = static_cast<int>(std::min<double>(std::ceil(std::min(h, maxSide)), imageHeight));
    // Shift (rather than shrink) a window that crosses an edge: the size stays
    // constant near the channel walls and the object stays inside it.
    const int x0 = std::clamp(static_cast<int>(std::floor(cx - iw / 2.0)), 0, imageWidth - iw);
    const int y0 = std::clamp(static_cast<int>(std::floor(cy - ih / 2.0)), 0, imageHeight - ih);
    return cv::Rect(x0, y0, std::max(iw, 1), std::max(ih, 1));
}

MonitoringCellStore::Entry MonitoringCellStore::copyCandidate(const MonitoringCellCandidate& c,
                                                              const std::string& noImageReason,
                                                              const MonitoringReasonContext* reasons) const
{
    Entry e;
    e.info.key = c.key;
    e.info.metrics = c.metrics;
    if (reasons && !c.metrics.valid) e.info.reasons = monitoringCellReasons(c.metrics, c.verdict, *reasons);
    if (c.image.empty()) {
        e.info.noImageReason = c.imageReleased ? kNoImageReleased : noImageReason;
        return e;
    }
    const cv::Mat gray = toMono8(c.image);
    if (gray.empty()) {
        e.info.noImageReason = kNoImageFormat;
        return e;
    }
    const cv::Rect rect = cropRect(gray.cols, gray.rows, c.metrics, policy_);
    e.image = gray(rect).clone(); // the owned copy: continuous, independent of the ring
    if (!c.mask.empty() && c.mask.size() == c.image.size()) {
        const cv::Mat mask = toMono8(c.mask);
        if (!mask.empty()) e.mask = mask(rect).clone();
    }
    e.info.hasImage = true;
    e.info.hasMask = !e.mask.empty();
    e.info.width = rect.width;
    e.info.height = rect.height;
    e.info.cropX = rect.x;
    e.info.cropY = rect.y;
    e.info.sourceWidth = gray.cols;
    e.info.sourceHeight = gray.rows;
    e.info.bytes = static_cast<std::uint64_t>(e.image.total()) + static_cast<std::uint64_t>(e.mask.total());
    return e;
}

void MonitoringCellStore::beginRun(std::uint64_t startGeneration)
{
    std::vector<cv::Mat> freed; // released when this returns, after the lock
    std::uint64_t epoch = 0;
    std::size_t pins = 0;
    {
        std::scoped_lock lk(mutex_);
        scope_ = startGeneration;
        epoch = advanceLocked(kRunStarted, std::nullopt, true, freed);
        pins = lastChange_->pinsDropped;
    }
    SPDLOG_INFO("MonitoringCellStore: capture {} begins: {} (run {}); {} pin(s) dropped", epoch, kRunStarted,
                startGeneration, pins);
}

std::uint64_t MonitoringCellStore::scopeGeneration() const
{
    std::scoped_lock lk(mutex_);
    return scope_;
}

std::uint64_t MonitoringCellStore::advanceLocked(const std::string& reason,
                                                 std::optional<std::uint64_t> cameraSessionFloor, bool dropPins,
                                                 std::vector<cv::Mat>& freed)
{
    // Never below what the source published (it may have moved on before this store existed).
    const std::uint64_t ended = current_;
    const std::uint64_t next = std::max(current_, source_.currentCaptureEpoch()) + 1;
    // Atomic stores: a frame entering processing from now on carries `next`; one that entered
    // before carries an earlier epoch and is refused at every insert below this mutex.
    source_.publishCapture(next, cameraSessionFloor);
    current_ = next;
    ++epoch_; // a refresh that read the old capture admits nothing
    ++revision_;
    CaptureChange change;
    change.captureEpoch = next;
    change.reason = reason;
    change.stripDropped = strip_.size();
    change.invalidStripDropped = invalid_.size();
    counters_.stripEvicted += strip_.size();
    counters_.invalidStripEvicted += invalid_.size();
    const auto release = [&freed](Entry& e) {
        if (!e.image.empty()) freed.push_back(std::move(e.image));
        if (!e.mask.empty()) freed.push_back(std::move(e.mask));
        e.image = cv::Mat();
        e.mask = cv::Mat();
    };
    for (auto& e : strip_) release(e);
    strip_.clear();
    stripBytes_ = 0;
    for (auto& e : invalid_) release(e);
    invalid_.clear();
    invalidBytes_ = 0;
    if (dropPins) {
        change.pinsDropped = pins_.size();
        for (auto& e : pins_) release(e);
        pins_.clear();
        pinBytes_ = 0;
    } else {
        // Pins belong to the capture that made them: those of the capture that ended keep their
        // identity and measurements, never its pixels.
        for (auto& e : pins_) {
            if (e.info.key.captureEpoch != ended) continue; // ended before (or never had an image)
            ++change.pinsEnded;
            if (!e.info.hasImage) continue; // keeps its own reason (left the buffer, results only)
            release(e);
            pinBytes_ -= std::min(pinBytes_, e.info.bytes);
            e.info.hasImage = false;
            e.info.hasMask = false;
            e.info.width = e.info.height = 0;
            e.info.cropX = e.info.cropY = 0;
            e.info.sourceWidth = e.info.sourceHeight = 0;
            e.info.bytes = 0;
            e.info.noImageReason = "no image: capture " + std::to_string(ended) + " ended (" + reason +
                                   "); the pin keeps its measurements";
        }
    }
    index_.clear();
    indexOrder_.clear();
    lastChange_ = std::move(change);
    return next;
}

std::uint64_t MonitoringCellStore::advanceCapture(const std::string& reason,
                                                  std::optional<std::uint64_t> cameraSessionFloor)
{
    std::vector<cv::Mat> freed; // released when this returns, after the lock
    std::uint64_t epoch = 0;
    std::size_t strip = 0, ended = 0;
    {
        std::scoped_lock lk(mutex_);
        epoch = advanceLocked(reason, cameraSessionFloor, false, freed);
        strip = lastChange_->stripDropped;
        ended = lastChange_->pinsEnded;
    }
    SPDLOG_INFO("MonitoringCellStore: capture {} begins: {}; {} strip cell(s) dropped, {} pin(s) kept without images",
                epoch, reason, strip, ended);
    return epoch;
}

std::uint64_t MonitoringCellStore::currentCapture() const
{
    std::scoped_lock lk(mutex_);
    return current_;
}

std::string MonitoringCellStore::captureRefusalLocked(const char* what, const MonitoringCellKey& key) const
{
    std::string m = std::string(what) + ": " + cellName(key.frameIndex, key.objectId);
    if (key.captureEpoch == 0)
        m += " is of no capture (its frame crossed a capture change while it was processed)";
    else
        m += " is from capture " + std::to_string(key.captureEpoch);
    m += ", but the current capture is " + std::to_string(current_);
    if (lastChange_ && lastChange_->captureEpoch == current_)
        m += " (" + lastChange_->reason + ")";
    else
        m += " (the camera, its mode, the frame buffer or the processing ROI changed)";
    return m + "; pins belong to the capture that made them, and a cell is never served across a capture change";
}

void MonitoringCellStore::clearStrip()
{
    std::deque<Entry> strip, invalid;
    {
        std::scoped_lock lk(mutex_);
        ++epoch_;
        if (!strip_.empty() || !invalid_.empty()) ++revision_;
        counters_.stripEvicted += strip_.size();
        counters_.invalidStripEvicted += invalid_.size();
        strip.swap(strip_);
        invalid.swap(invalid_);
        stripBytes_ = 0;
        invalidBytes_ = 0;
        index_.clear();
        indexOrder_.clear();
    }
}

void MonitoringCellStore::indexLocked(const MonitoringCellKey& key, const MonitoringCellMetrics& m)
{
    if (policy_.rowIndexMax == 0) return;
    auto [it, inserted] = index_.try_emplace(key, m);
    if (!inserted) {
        it->second = m;
        return;
    }
    indexOrder_.push_back(key);
    while (indexOrder_.size() > policy_.rowIndexMax) {
        index_.erase(indexOrder_.front());
        indexOrder_.pop_front();
    }
}

void MonitoringCellStore::trimStripLocked()
{
    while (!strip_.empty() && (strip_.size() > policy_.stripMaxCells || stripBytes_ > policy_.stripMaxBytes)) {
        stripBytes_ -= std::min(stripBytes_, strip_.front().info.bytes);
        strip_.pop_front();
        ++counters_.stripEvicted;
        ++revision_;
    }
    while (!invalid_.empty() &&
           (invalid_.size() > policy_.invalidStripMaxCells || invalidBytes_ > policy_.invalidStripMaxBytes)) {
        invalidBytes_ -= std::min(invalidBytes_, invalid_.front().info.bytes);
        invalid_.pop_front();
        ++counters_.invalidStripEvicted;
        ++revision_;
    }
}

void MonitoringCellStore::raisePeakLocked()
{
    peakBytes_ = std::max(peakBytes_, stripBytes_ + invalidBytes_ + pinBytes_);
}

const MonitoringCellStore::Entry* MonitoringCellStore::findLocked(const MonitoringCellKey& key, bool pinsFirst) const
{
    auto inPins = [&]() -> const Entry* {
        for (const auto& e : pins_)
            if (e.info.key == key) return &e;
        return nullptr;
    };
    // The strips (valid, then invalid): a row is in at most one of them (its validity is fixed).
    auto inStrip = [&]() -> const Entry* {
        for (auto it = strip_.rbegin(); it != strip_.rend(); ++it)
            if (it->info.key == key) return &*it;
        for (auto it = invalid_.rbegin(); it != invalid_.rend(); ++it)
            if (it->info.key == key) return &*it;
        return nullptr;
    };
    if (pinsFirst) {
        if (const auto* e = inPins()) return e;
        return inStrip();
    }
    return inStrip();
}

void MonitoringCellStore::refreshStrip()
{
    const auto t0 = Clock::now();
    std::uint64_t epoch = 0;
    std::uint64_t current = 0;
    std::vector<MonitoringCellKey> held, heldInvalid;
    {
        std::scoped_lock lk(mutex_);
        epoch = epoch_;
        current = current_;
        held.reserve(strip_.size());
        for (const auto& e : strip_) held.push_back(e.info.key);
        heldInvalid.reserve(invalid_.size());
        for (const auto& e : invalid_) heldInvalid.push_back(e.info.key);
    }
    // The ring's lock is held inside this call only (headers, no pixels), once for both halves.
    auto newest = source_.newestRows(policy_.stripMaxCells, policy_.invalidStripMaxCells);
    const auto tSource = Clock::now();
    std::vector<Entry> fresh, freshInvalid;
    fresh.reserve(newest.rows.size());
    freshInvalid.reserve(newest.invalidRows.size());
    std::size_t otherCapture = 0;
    const auto inRun = [&newest](const MonitoringCellKey& k) { return k.startGeneration == newest.ringGeneration; };
    for (const auto& c : newest.rows) {
        if (!inRun(c.key)) continue; // another run's (or none's)
        if (c.key.captureEpoch != current) { // its frame entered with another capture: never a cell
            ++otherCapture;
            continue;
        }
        if (std::find(held.begin(), held.end(), c.key) != held.end()) continue;
        fresh.push_back(copyCandidate(c, kNoImageResultsOnly)); // crop + clone, no lock held
    }
    // The invalid half: the same rules, plus the reasons (the config is read once, only when a
    // new invalid row is admitted, outside every lock).
    std::optional<MonitoringReasonContext> reasonContext;
    for (const auto& c : newest.invalidRows) {
        if (!inRun(c.key)) continue;
        if (c.key.captureEpoch != current) {
            ++otherCapture;
            continue;
        }
        if (std::find(heldInvalid.begin(), heldInvalid.end(), c.key) != heldInvalid.end()) continue;
        if (!reasonContext) reasonContext = source_.reasonContext();
        freshInvalid.push_back(copyCandidate(c, kNoImageResultsOnly, &*reasonContext));
    }
    // One return of the shares for both halves; keys and metrics stay for the index below.
    newest.rows.insert(newest.rows.end(), std::make_move_iterator(newest.invalidRows.begin()),
                       std::make_move_iterator(newest.invalidRows.end()));
    newest.invalidRows.clear();
    source_.releaseShares(newest.rows);
    std::deque<Entry> dropped; // freed when this returns, after the lock
    std::function<void(const char*)> droppedHook; // tests only
    {
        std::scoped_lock lk(mutex_);
        ++counters_.refreshes;
        counters_.lastSourceUs = elapsedUs(t0, tSource);
        if (epoch != epoch_) {
            // A capture change (a run start among them) or a clear happened meanwhile: this batch is stale.
            counters_.lastRefreshUs = elapsedUs(t0, Clock::now());
            return;
        }
        if (ringGeneration_ != newest.ringGeneration) {
            ringGeneration_ = newest.ringGeneration;
            ++revision_;
        }
        for (auto it = strip_.begin(); it != strip_.end();) {
            if (!inRun(it->info.key)) {
                stripBytes_ -= std::min(stripBytes_, it->info.bytes);
                dropped.push_back(std::move(*it));
                it = strip_.erase(it);
                ++counters_.stripEvicted;
                ++revision_;
            } else {
                ++it;
            }
        }
        for (auto it = invalid_.begin(); it != invalid_.end();) {
            if (!inRun(it->info.key)) {
                invalidBytes_ -= std::min(invalidBytes_, it->info.bytes);
                dropped.push_back(std::move(*it));
                it = invalid_.erase(it);
                ++counters_.invalidStripEvicted;
                ++revision_;
            } else {
                ++it;
            }
        }
        for (auto& e : fresh) {
            // The insert rule, under the mutex the capture changes take: the row's carried capture
            // is the current one.
            if (e.info.key.captureEpoch != current_) {
                ++otherCapture;
                continue;
            }
            if (findLocked(e.info.key, false)) continue; // another refresh admitted it
            stripBytes_ += e.info.bytes;
            strip_.push_back(std::move(e));
            ++counters_.stripAdmitted;
            ++revision_;
        }
        for (auto& e : freshInvalid) {
            if (e.info.key.captureEpoch != current_) {
                ++otherCapture;
                continue;
            }
            if (findLocked(e.info.key, false)) continue;
            invalidBytes_ += e.info.bytes;
            invalid_.push_back(std::move(e));
            ++counters_.invalidStripAdmitted;
            ++revision_;
        }
        for (const auto& c : newest.rows)
            if (c.key.captureEpoch == current_) indexLocked(c.key, c.metrics);
        trimStripLocked();
        raisePeakLocked();
        counters_.lastRefreshUs = elapsedUs(t0, Clock::now());
        if (readHook_ && !dropped.empty()) droppedHook = readHook_;
    }
    if (droppedHook) droppedHook("refresh-dropped"); // after the lock, the dropped copies still held
    if (otherCapture)
        SPDLOG_DEBUG("MonitoringCellStore: {} row(s) of another capture than {} not admitted to the strip",
                     otherCapture, current);
}

MonitoringCellStore::PinResult MonitoringCellStore::pin(const MonitoringCellKey& key)
{
    const std::uint64_t expectedStartGeneration = key.startGeneration;
    const std::string cell = cellName(key.frameIndex, key.objectId);
    PinResult r;
    auto refuse = [&r](PinOutcome outcome, std::string message) {
        r.outcome = outcome;
        r.message = std::move(message);
        return r;
    };
    // Admission under the lock; the scope and the capture must still be the ones checked first.
    auto admit = [&](Entry&& e, std::uint64_t scope) -> PinResult {
        std::scoped_lock lk(mutex_);
        // The insert rule, under the mutex the capture changes take: the row's carried capture is
        // the current one (a change since the check below refuses it).
        if (e.info.key.captureEpoch != current_) {
            SPDLOG_DEBUG("MonitoringCellStore: pin of frame {} object {} (capture {}) refused: capture {} began",
                         key.frameIndex, key.objectId, e.info.key.captureEpoch, current_);
            return refuse(PinOutcome::CaptureMismatch,
                          captureRefusalLocked("pin refused: the capture changed while the pin was taken", key));
        }
        if (scope_ != scope)
            return refuse(PinOutcome::RunMismatch, "pin refused: run " + std::to_string(scope_) +
                                                       " started while the pin was taken; pins now belong to it");
        for (const auto& p : pins_) {
            if (p.info.key == key) { // another caller pinned it meanwhile
                r.cell = p.info;
                return refuse(PinOutcome::AlreadyPinned, "already pinned");
            }
        }
        if (pins_.size() >= policy_.maxPins)
            return refuse(PinOutcome::LimitReached, "pin refused: " + std::to_string(policy_.maxPins) +
                                                        " cells are pinned (the limit); unpin one first");
        if (pinBytes_ + e.info.bytes > policy_.pinMaxBytes)
            return refuse(PinOutcome::LimitReached, "pin refused: the pinned copies would exceed " +
                                                        std::to_string(policy_.pinMaxBytes) + " bytes; unpin one first");
        r.outcome = e.info.hasImage ? PinOutcome::Pinned : PinOutcome::PinnedWithoutImage;
        r.message = e.info.hasImage ? "pinned" : e.info.noImageReason;
        r.cell = e.info;
        pinBytes_ += e.info.bytes;
        pins_.push_back(std::move(e));
        ++revision_;
        raisePeakLocked();
        return r;
    };

    // The capture first, before any other rule: an identity of another capture is never resolved.
    std::uint64_t scope = 0;
    {
        std::scoped_lock lk(mutex_);
        if (key.captureEpoch != current_)
            return refuse(PinOutcome::CaptureMismatch, captureRefusalLocked("pin refused", key));
        scope = scope_;
        if (expectedStartGeneration != 0 && expectedStartGeneration != scope_) {
            return refuse(PinOutcome::RunMismatch,
                          "pin refused: run " + std::to_string(expectedStartGeneration) +
                              " is not the latest run started (" +
                              (scope_ == 0 ? std::string("none has started") : "run " + std::to_string(scope_)) +
                              "); its pins were dropped when the next run started");
        }
        for (const auto& p : pins_) {
            if (p.info.key == key) {
                r.cell = p.info;
                return refuse(PinOutcome::AlreadyPinned, "already pinned");
            }
        }
        if (pins_.size() >= policy_.maxPins)
            return refuse(PinOutcome::LimitReached, "pin refused: " + std::to_string(policy_.maxPins) +
                                                        " cells are pinned (the limit); unpin one first");
    }
    // The ring (headers under its lock): only a row whose frame carried this capture is this
    // identity (frame indices restart with a capture); admit() rechecks the capture.
    auto found = source_.find(key.frameIndex, key.objectId);
    // The strip already holds an owned copy of this capture's row: the pin takes a copy of that
    // copy (no new crop), under the lock, never a refcount share of it (#789). Every pixel buffer
    // has exactly one owner in the store, so whichever thread removes it under the mutex frees it
    // after every read (reads happen under the mutex only); a share would make the last release
    // whichever owner went last, ordered after the readers by cv::Mat's reference count alone,
    // which ThreadSanitizer cannot see (it lives in the uninstrumented OpenCV library).
    {
        std::optional<Entry> fromStrip;
        {
            std::scoped_lock lk(mutex_);
            if (const auto* s = findLocked(key, false)) {
                Entry e;
                e.info = s->info;
                e.image = s->image.clone(); // ≤ crop_max_side² bytes each; empty stays empty
                e.mask = s->mask.clone();
                fromStrip = std::move(e);
            }
        }
        if (fromStrip) {
            source_.releaseShares(found.rows);
            return admit(std::move(*fromStrip), scope);
        }
    }
    // An owned crop of the ring's row, outside every lock.
    std::optional<Entry> copy;
    std::optional<std::uint64_t> otherGeneration;
    for (const auto& c : found.rows) {
        if (c.key.captureEpoch != key.captureEpoch) continue; // another capture's frame: unrelated
        if (c.key.startGeneration == expectedStartGeneration) {
            std::optional<MonitoringReasonContext> reasons; // an invalid row's reasons, outside every lock
            if (!c.metrics.valid) reasons = source_.reasonContext();
            copy = copyCandidate(c, kNoImageResultsOnly, reasons ? &*reasons : nullptr);
            break;
        }
        if (!otherGeneration) otherGeneration = c.key.startGeneration;
    }
    source_.releaseShares(found.rows);
    if (!copy) {
        std::optional<MonitoringCellMetrics> known;
        {
            std::scoped_lock lk(mutex_);
            if (auto it = index_.find(key); it != index_.end()) known = it->second;
        }
        if (known) {
            Entry e;
            e.info.key = key;
            e.info.metrics = *known;
            e.info.noImageReason =
                "no image: the frame left the monitoring buffer before it was pinned (the buffer keeps the newest " +
                std::to_string(ProcessingService::getMonitoringCapacity()) +
                " valid and invalid detections); the measurements are those the monitoring snapshot reported";
            copy = std::move(e);
        } else if (otherGeneration) {
            return refuse(PinOutcome::RunMismatch, "pin refused: " + cell + " was detected " + runName(*otherGeneration) +
                                                       ", not " + runName(expectedStartGeneration));
        } else {
            return refuse(PinOutcome::NotFound, "pin refused: " + cell + " " + runName(expectedStartGeneration) +
                                                    " is not held: it left the monitoring buffer and no monitoring "
                                                    "snapshot reported it");
        }
    }
    return admit(std::move(*copy), scope);
}

MonitoringCellStore::ReadResult MonitoringCellStore::unpin(const MonitoringCellKey& key)
{
    std::optional<Entry> released; // freed after the lock is released
    std::scoped_lock lk(mutex_);
    for (auto it = pins_.begin(); it != pins_.end(); ++it) {
        if (it->info.key == key) { // a pin of an ended capture too: it is the operator's to remove
            pinBytes_ -= std::min(pinBytes_, it->info.bytes);
            released = std::move(*it);
            pins_.erase(it);
            ++revision_;
            return {true, false, "unpinned"};
        }
    }
    if (key.captureEpoch != current_)
        return {false, true, captureRefusalLocked("not pinned (pins were dropped when a run started)", key)};
    return {false, false, {}};
}

std::size_t MonitoringCellStore::clearPins()
{
    std::vector<Entry> released;
    std::scoped_lock lk(mutex_);
    const std::size_t n = pins_.size();
    released.swap(pins_);
    pinBytes_ = 0;
    if (n) ++revision_;
    return n;
}

void MonitoringCellStore::noteRows(const std::vector<std::pair<MonitoringCellKey, MonitoringCellMetrics>>& rows)
{
    std::scoped_lock lk(mutex_);
    for (const auto& [key, metrics] : rows)
        if (key.captureEpoch == current_) indexLocked(key, metrics); // another capture's row is never pinned
}

void MonitoringCellStore::setReadHookForTests(std::function<void(const char* where)> hook)
{
    std::scoped_lock lk(mutex_);
    readHook_ = std::move(hook);
}

MonitoringCellStore::Listing MonitoringCellStore::listing() const
{
    Listing out;
    // One critical section: the current capture, then the cells (a capture change waits for it).
    std::unique_lock lk(mutex_);
    out.ringCaptureEpoch = current_;
    if (readHook_) readHook_("listing");
    out.scopeGeneration = scope_;
    out.ringGeneration = ringGeneration_;
    out.revision = revision_;
    out.strip.reserve(strip_.size());
    for (const auto& e : strip_)
        if (e.info.key.captureEpoch == out.ringCaptureEpoch) out.strip.push_back(e.info);
    out.invalidStrip.reserve(invalid_.size());
    for (const auto& e : invalid_)
        if (e.info.key.captureEpoch == out.ringCaptureEpoch) out.invalidStrip.push_back(e.info);
    out.pins.reserve(pins_.size());
    for (const auto& e : pins_) out.pins.push_back(e.info); // an ended capture's pin has no image
    out.lastCaptureChange = lastChange_;
    out.stats = counters_;
    out.stats.stripCells = strip_.size();
    out.stats.stripBytes = stripBytes_;
    out.stats.invalidStripCells = invalid_.size();
    out.stats.invalidStripBytes = invalidBytes_;
    out.stats.pins = pins_.size();
    out.stats.pinBytes = pinBytes_;
    out.stats.indexedRows = index_.size();
    out.stats.peakBytes = peakBytes_;
    out.policy = policy_;
    return out;
}

MonitoringCellStore::ReadResult MonitoringCellStore::image(const MonitoringCellKey& key, bool mask, cv::Mat& out,
                                                           MonitoringCellInfo* info) const
{
    // One critical section: the capture check, the lookup and the copy (a capture change waits for it).
    std::unique_lock lk(mutex_);
    // The capture first: whatever the store still holds, another capture's cell is never served.
    if (key.captureEpoch != current_) return {false, true, captureRefusalLocked("image refused", key)};
    const Entry* e = findLocked(key, true);
    if (!e)
        return {false, false,
                "not held: " + cellName(key.frameIndex, key.objectId) + " " + runName(key.startGeneration) +
                    " is neither pinned nor in the strip"};
    const cv::Mat layer = mask ? e->mask : e->image; // a share of the owned copy, dropped under the lock
    if (layer.empty())
        return {false, false,
                !e->info.hasImage ? e->info.noImageReason
                                  : "no mask: " + cellName(key.frameIndex, key.objectId) + " has no processed mask"};
    if (info) *info = e->info;
    if (readHook_) readHook_("image"); // resolved; the copy follows
    out = layer.clone(); // a private copy, under the lock (see the header)
    return {true, false, {}};
}

MonitoringCellStore::Stats MonitoringCellStore::stats() const
{
    return listing().stats;
}

diagnostics::MemoryOwnerStats MonitoringCellStore::memoryStats() const
{
    diagnostics::MemoryOwnerStats o;
    o.name = "monitoring.cellStore";
    o.knowledge = diagnostics::MemoryKnowledge::Measured;
    std::scoped_lock lk(mutex_);
    o.currentBytes = stripBytes_ + invalidBytes_ + pinBytes_;
    o.peakBytes = peakBytes_;
    o.currentCount = strip_.size() + invalid_.size() + pins_.size();
    o.peakCount = o.currentCount;
    o.capacityBytes = policy_.stripMaxBytes + policy_.invalidStripMaxBytes + policy_.pinMaxBytes;
    o.capacityCount = policy_.stripMaxCells + policy_.invalidStripMaxCells + policy_.maxPins;
    o.evictedByBudget = counters_.stripEvicted + counters_.invalidStripEvicted;
    o.note = "owned crops of the live valid and invalid detected-cell strips and the pinned cells (bridge ABI 41, "
             "invalid strip ABI next (#880, set at merge)); a pinned strip cell has a copy of its own in each (#789)";
    return o;
}

MonitoringCellSource::Rows MonitoringCellSource::newestInvalid(std::size_t)
{
    return {};
}

MonitoringCellSource::Rows MonitoringCellSource::newestRows(std::size_t maxValid, std::size_t maxInvalid)
{
    Rows out = newestValid(maxValid);
    Rows invalid = newestInvalid(maxInvalid);
    out.invalidRows = std::move(invalid.rows);
    return out;
}

MonitoringReasonContext MonitoringCellSource::reasonContext() const
{
    return {};
}

void MonitoringCellSource::releaseShares(std::vector<MonitoringCellCandidate>& rows)
{
    for (auto& c : rows) {
        c.image.release();
        c.mask.release();
    }
}

// ---------------------------------------------------------------------------
// The production source: the monitoring ring of ProcessingService.

namespace {

class ProcessingCellSource final : public MonitoringCellSource {
public:
    explicit ProcessingCellSource(ProcessingService& processing) : processing_(processing) {}

    std::uint64_t currentCaptureEpoch() const override { return processing_.monitoringCaptureEpoch(); }

    void publishCapture(std::uint64_t captureEpoch, std::optional<std::uint64_t> cameraSessionFloor) override
    {
        processing_.publishMonitoringCapture(captureEpoch, cameraSessionFloor); // atomic stores
    }

    Rows newestValid(std::size_t maxRows) override { return read(maxRows, 0); }

    Rows newestInvalid(std::size_t maxRows) override
    {
        Rows out = read(0, maxRows);
        out.rows = std::move(out.invalidRows);
        out.invalidRows.clear();
        return out;
    }

    Rows newestRows(std::size_t maxValid, std::size_t maxInvalid) override { return read(maxValid, maxInvalid); }

    MonitoringReasonContext reasonContext() const override
    {
        // Reads under the config's own mutex, never under the ring's lock or the store's.
        MonitoringReasonContext c;
        c.config = processing_.getEffectiveProcessingConfig();
        c.pixelToMicron = processing_.getPixelToMicronFactor();
        return c;
    }

    Rows find(std::uint64_t frameIndex, int objectId) override
    {
        Rows out;
        const auto view = processing_.visitMonitoringNewest(
            std::numeric_limits<std::size_t>::max(), true,
            [&](const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& tag, bool) {
                if (f.index != frameIndex || f.validation.objectId != objectId) return;
                out.rows.push_back(candidate(f, tag));
            });
        std::reverse(out.rows.begin(), out.rows.end()); // newest first
        out.ringGeneration = view.runGeneration;
        return out;
    }

    void releaseShares(std::vector<MonitoringCellCandidate>& rows) override
    {
        std::vector<cv::Mat> shares;
        shares.reserve(rows.size() * 2);
        for (auto& c : rows) {
            if (!c.image.empty()) shares.push_back(std::move(c.image));
            if (!c.mask.empty()) shares.push_back(std::move(c.mask));
            c.image = cv::Mat();
            c.mask = cv::Mat();
        }
        if (!shares.empty()) processing_.releaseMonitoringShares(shares);
    }

private:
    // The newest rows of the two rings under one hold of the ring's lock.
    Rows read(std::size_t maxValid, std::size_t maxInvalid)
    {
        Rows out;
        out.rows.reserve(maxValid); // allocated before the lock is taken
        out.invalidRows.reserve(maxInvalid);
        const auto view = processing_.visitMonitoringNewestPerRing(
            maxValid, maxInvalid,
            [&out](const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& tag, bool valid) {
                (valid ? out.rows : out.invalidRows).push_back(candidate(f, tag));
            });
        out.ringGeneration = view.runGeneration;
        return out;
    }

    // Under the ring's lock: scalars and Mat headers; geometry into image (ROI) coordinates. An
    // invalid row also carries the handful of scalars the classifier reads.
    static MonitoringCellCandidate candidate(const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& tag)
    {
        MonitoringCellCandidate c{
            {tag.runGeneration, f.index, f.validation.objectId, tag.captureEpoch},
            monitoringCellMetrics(f.validation, f.timestampNs, {tag.geometryOriginX, tag.geometryOriginY}),
            {},
            f.originalImage, f.processedImage, tag.imagesReleased};
        if (!f.validation.isValid) {
            const auto& v = f.validation;
            c.verdict.touchesBorder = v.touchesBorder;
            c.verdict.inChannel = v.inChannel;
            c.verdict.degenerateContour = v.degenerateContour;
            c.verdict.innerContourCount = v.innerContourCount;
            c.verdict.laplacianVariance = v.laplacianVariance;
            c.verdict.centroidRow = v.centroidY;
        }
        return c;
    }

    ProcessingService& processing_;
};

} // namespace

std::unique_ptr<MonitoringCellSource> makeProcessingCellSource(ProcessingService& processing)
{
    return std::make_unique<ProcessingCellSource>(processing);
}

} // namespace backend::services
