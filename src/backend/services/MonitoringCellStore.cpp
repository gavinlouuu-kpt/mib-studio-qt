#include "backend/services/MonitoringCellStore.h"

#include "backend/processing/ProcessingService.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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
constexpr const char* kNoImageFormat = "no image: the frame's pixel format is not 8- or 16-bit";

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

MonitoringCellMetrics monitoringCellMetrics(const FilterResult& v, std::uint64_t timestampNs)
{
    MonitoringCellMetrics m;
    m.timestampNs = timestampNs;
    m.valid = v.isValid;
    m.targetGroup = v.isTargetGroup;
    m.objectCount = v.objectCount;
    m.trackId = v.trackId;
    m.centroidX = v.centroidX;
    m.centroidY = v.centroidY;
    m.bboxX = v.bboxX;
    m.bboxY = v.bboxY;
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

MonitoringCellStore::MonitoringCellStore(MonitoringCellSource& source) : MonitoringCellStore(source, Policy{}) {}

MonitoringCellStore::MonitoringCellStore(MonitoringCellSource& source, Policy policy)
    : source_(source), policy_(policy)
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
               m.centroidX < imageWidth && m.centroidY < imageHeight) {
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
                                                              const std::string& noImageReason) const
{
    Entry e;
    e.info.key = c.key;
    e.info.metrics = c.metrics;
    if (c.image.empty()) {
        e.info.noImageReason = noImageReason;
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
    std::deque<Entry> strip;
    std::vector<Entry> pins;
    {
        std::scoped_lock lk(mutex_);
        scope_ = startGeneration;
        ++epoch_;
        ++revision_;
        counters_.stripEvicted += strip_.size();
        strip.swap(strip_);
        pins.swap(pins_);
        stripBytes_ = 0;
        pinBytes_ = 0;
        index_.clear();
        indexOrder_.clear();
    }
    // The copies are released here, outside the lock.
}

std::uint64_t MonitoringCellStore::scopeGeneration() const
{
    std::scoped_lock lk(mutex_);
    return scope_;
}

void MonitoringCellStore::clearStrip()
{
    std::deque<Entry> strip;
    {
        std::scoped_lock lk(mutex_);
        ++epoch_;
        if (!strip_.empty()) ++revision_;
        counters_.stripEvicted += strip_.size();
        strip.swap(strip_);
        stripBytes_ = 0;
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
}

void MonitoringCellStore::raisePeakLocked()
{
    peakBytes_ = std::max(peakBytes_, stripBytes_ + pinBytes_);
}

const MonitoringCellStore::Entry* MonitoringCellStore::findLocked(const MonitoringCellKey& key, bool pinsFirst) const
{
    auto inPins = [&]() -> const Entry* {
        for (const auto& e : pins_)
            if (e.info.key == key) return &e;
        return nullptr;
    };
    auto inStrip = [&]() -> const Entry* {
        for (auto it = strip_.rbegin(); it != strip_.rend(); ++it)
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
    std::vector<MonitoringCellKey> held;
    {
        std::scoped_lock lk(mutex_);
        epoch = epoch_;
        held.reserve(strip_.size());
        for (const auto& e : strip_) held.push_back(e.info.key);
    }
    // The ring's lock is held inside this call only (headers, no pixels).
    auto newest = source_.newestValid(policy_.stripMaxCells);
    const auto tSource = Clock::now();
    std::vector<Entry> fresh;
    fresh.reserve(newest.rows.size());
    for (const auto& c : newest.rows) {
        if (c.key.startGeneration != newest.ringGeneration) continue; // another context
        if (std::find(held.begin(), held.end(), c.key) != held.end()) continue;
        fresh.push_back(copyCandidate(c, kNoImageResultsOnly)); // crop + clone, no lock held
    }
    std::deque<Entry> dropped;
    {
        std::scoped_lock lk(mutex_);
        ++counters_.refreshes;
        counters_.lastSourceUs = elapsedUs(t0, tSource);
        if (epoch != epoch_) {
            // A run started or the ring was cleared meanwhile: this batch is stale.
            counters_.lastRefreshUs = elapsedUs(t0, Clock::now());
            return;
        }
        if (ringGeneration_ != newest.ringGeneration) {
            ringGeneration_ = newest.ringGeneration;
            ++revision_;
        }
        for (auto it = strip_.begin(); it != strip_.end();) {
            if (it->info.key.startGeneration != newest.ringGeneration) {
                stripBytes_ -= std::min(stripBytes_, it->info.bytes);
                dropped.push_back(std::move(*it));
                it = strip_.erase(it);
                ++counters_.stripEvicted;
                ++revision_;
            } else {
                ++it;
            }
        }
        for (auto& e : fresh) {
            if (findLocked(e.info.key, false)) continue; // another refresh admitted it
            stripBytes_ += e.info.bytes;
            strip_.push_back(std::move(e));
            ++counters_.stripAdmitted;
            ++revision_;
        }
        for (const auto& c : newest.rows) indexLocked(c.key, c.metrics);
        trimStripLocked();
        raisePeakLocked();
        counters_.lastRefreshUs = elapsedUs(t0, Clock::now());
    }
}

MonitoringCellStore::PinResult MonitoringCellStore::pin(std::uint64_t expectedStartGeneration,
                                                        std::uint64_t frameIndex, int objectId)
{
    const MonitoringCellKey key{expectedStartGeneration, frameIndex, objectId};
    const std::string cell = cellName(frameIndex, objectId);
    PinResult r;
    auto refuse = [&r](PinOutcome outcome, std::string message) {
        r.outcome = outcome;
        r.message = std::move(message);
        return r;
    };
    // Admission under the lock; the scope must still be the one checked first.
    auto admit = [&](Entry&& e, std::uint64_t scope) -> PinResult {
        std::scoped_lock lk(mutex_);
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

    std::uint64_t scope = 0;
    {
        std::scoped_lock lk(mutex_);
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
    // The strip already holds an owned copy: share it (immutable), no new crop.
    {
        std::optional<Entry> fromStrip;
        {
            std::scoped_lock lk(mutex_);
            if (const auto* s = findLocked(key, false)) fromStrip = *s;
        }
        if (fromStrip) return admit(std::move(*fromStrip), scope);
    }
    // The ring (headers under its lock), then an owned crop outside every lock.
    std::optional<Entry> copy;
    std::optional<std::uint64_t> otherGeneration;
    for (const auto& c : source_.find(frameIndex, objectId)) {
        if (c.key.startGeneration == expectedStartGeneration) {
            copy = copyCandidate(c, kNoImageResultsOnly);
            break;
        }
        if (!otherGeneration) otherGeneration = c.key.startGeneration;
    }
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

bool MonitoringCellStore::unpin(const MonitoringCellKey& key)
{
    std::optional<Entry> released;
    std::scoped_lock lk(mutex_);
    for (auto it = pins_.begin(); it != pins_.end(); ++it) {
        if (it->info.key == key) {
            pinBytes_ -= std::min(pinBytes_, it->info.bytes);
            released = std::move(*it);
            pins_.erase(it);
            ++revision_;
            return true;
        }
    }
    return false;
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
    for (const auto& [key, metrics] : rows) indexLocked(key, metrics);
}

MonitoringCellStore::Listing MonitoringCellStore::listing() const
{
    Listing out;
    std::scoped_lock lk(mutex_);
    out.scopeGeneration = scope_;
    out.ringGeneration = ringGeneration_;
    out.revision = revision_;
    out.strip.reserve(strip_.size());
    for (const auto& e : strip_) out.strip.push_back(e.info);
    out.pins.reserve(pins_.size());
    for (const auto& e : pins_) out.pins.push_back(e.info);
    out.stats = counters_;
    out.stats.stripCells = strip_.size();
    out.stats.stripBytes = stripBytes_;
    out.stats.pins = pins_.size();
    out.stats.pinBytes = pinBytes_;
    out.stats.indexedRows = index_.size();
    out.stats.peakBytes = peakBytes_;
    out.policy = policy_;
    return out;
}

bool MonitoringCellStore::image(const MonitoringCellKey& key, bool mask, cv::Mat& out, MonitoringCellInfo* info) const
{
    std::scoped_lock lk(mutex_);
    const Entry* e = findLocked(key, true);
    if (!e) return false;
    const cv::Mat& m = mask ? e->mask : e->image;
    if (m.empty()) return false;
    out = m; // a share of the immutable copy; the caller copies pixels after unlocking
    if (info) *info = e->info;
    return true;
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
    o.currentBytes = stripBytes_ + pinBytes_;
    o.peakBytes = peakBytes_;
    o.currentCount = strip_.size() + pins_.size();
    o.peakCount = o.currentCount;
    o.capacityBytes = policy_.stripMaxBytes + policy_.pinMaxBytes;
    o.capacityCount = policy_.stripMaxCells + policy_.maxPins;
    o.evictedByBudget = counters_.stripEvicted;
    o.note = "owned crops of the live detected-cell strip and the pinned cells (bridge ABI 41); a strip shared by a "
             "pin is counted twice (upper bound)";
    return o;
}

// ---------------------------------------------------------------------------
// The production source: the monitoring ring of ProcessingService.

namespace {

class ProcessingCellSource final : public MonitoringCellSource {
public:
    explicit ProcessingCellSource(const ProcessingService& processing) : processing_(processing) {}

    Newest newestValid(std::size_t maxRows) override
    {
        Newest out;
        out.rows.reserve(maxRows); // allocated before the lock is taken
        const auto view = processing_.visitMonitoringNewest(
            maxRows, false, [&out](const ProcessedFrame& f, std::uint64_t gen, bool) {
                out.rows.push_back(MonitoringCellCandidate{{gen, f.index, f.validation.objectId},
                                                           monitoringCellMetrics(f.validation, f.timestampNs),
                                                           f.originalImage, f.processedImage});
            });
        out.ringGeneration = view.runGeneration;
        return out;
    }

    std::vector<MonitoringCellCandidate> find(std::uint64_t frameIndex, int objectId) override
    {
        std::vector<MonitoringCellCandidate> out;
        processing_.visitMonitoringNewest(
            std::numeric_limits<std::size_t>::max(), true,
            [&](const ProcessedFrame& f, std::uint64_t gen, bool) {
                if (f.index != frameIndex || f.validation.objectId != objectId) return;
                out.push_back(MonitoringCellCandidate{{gen, f.index, f.validation.objectId},
                                                      monitoringCellMetrics(f.validation, f.timestampNs),
                                                      f.originalImage, f.processedImage});
            });
        std::reverse(out.begin(), out.end()); // newest first
        return out;
    }

private:
    const ProcessingService& processing_;
};

} // namespace

std::unique_ptr<MonitoringCellSource> makeProcessingCellSource(const ProcessingService& processing)
{
    return std::make_unique<ProcessingCellSource>(processing);
}

} // namespace backend::services
