// monitoring_cell_store_test (#716 A6a, bridge ABI 41)
//
// The live cell store against a scripted ring (no pipeline):
//   1. crop policy: bounding box + margin, capped, shifted inside the image;
//   2. strip: newest valid rows of the ring's context, owned copies, oldest
//      evicted first at the count and the byte cap, every eviction counted, a
//      context change drops the other context's cells;
//   3. pins: an owned copy that survives the ring's eviction and the strip's,
//      never aliasing the ring's pixels; AlreadyPinned; the count and byte caps
//      refuse (an older pin is never evicted);
//   4. run rules: a stale or future generation is refused, a row of another
//      run (or of none) is refused, pins and the strip are dropped at the next
//      run start, a run starting while a pin is taken refuses it;
//   5. no image: an evicted row the snapshot reported keeps its measurements
//      with the reason, a row without pixels (PL) likewise, an unknown row is
//      NotFound; nothing is ever substituted;
//   6. unpin / clear pins / clear strip (pins stay);
//   7. shares: the store gives every share of the ring's images back to the
//      source once it has cropped (the production source drops them under the
//      ring's lock), and image() hands out private copies, never a share;
//   8. captures (review rounds 1-2): a capture change restarts frame indices,
//      so the same (run, frame, object) recurs. Pins belong to the capture
//      that made them. Before the store hears of the change (the append-path
//      backstop), every read path already refuses the earlier capture's
//      identities: image of a strip cell or a pin, pin (before AlreadyPinned),
//      unpin, and the listing lists none of them; the first pin or refresh of
//      the new capture drops them; a pin of the new capture gets the new
//      capture's pixels;
//   9. capture notifications (captureChanged, the listener path): the strip,
//      the pins and the index of the earlier capture are dropped at once, the
//      reason is kept for the listing and the refusals, late or repeated
//      notifications change nothing, cells of a newer capture are kept, and a
//      refresh under way when the capture changes admits nothing.

#include "backend/services/MonitoringCellStore.h"

#include "support/assert.h"

#include <opencv2/core.hpp>

#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using backend::services::MonitoringCellCandidate;
using backend::services::MonitoringCellInfo;
using backend::services::MonitoringCellKey;
using backend::services::MonitoringCellMetrics;
using backend::services::MonitoringCellSource;
using backend::services::MonitoringCellStore;
using Outcome = MonitoringCellStore::PinOutcome;

namespace {

constexpr int kW = 200;
constexpr int kH = 80;

// A scripted monitoring ring: rows appended in order, bounded like the real
// one (oldest dropped), each with its own freshly allocated image and mask.
class FakeRing final : public MonitoringCellSource {
public:
    explicit FakeRing(std::size_t capacity = 1000) : capacity_(capacity) {}

    // A row with a recognisable image: every pixel = `value`; the mask marks the box.
    MonitoringCellCandidate& append(std::uint64_t frameIndex, int objectId, bool valid, std::uint8_t value,
                                    bool withImage = true)
    {
        MonitoringCellCandidate c;
        c.key = {generation, frameIndex, objectId, captureEpoch};
        c.metrics.valid = valid;
        c.metrics.timestampNs = frameIndex * 1000;
        c.metrics.bboxX = 40;
        c.metrics.bboxY = 30;
        c.metrics.bboxWidth = 20;
        c.metrics.bboxHeight = 16;
        c.metrics.centroidX = 50;
        c.metrics.centroidY = 38;
        c.metrics.area = 250.0 + static_cast<double>(frameIndex);
        c.metrics.deformability = 0.05;
        if (withImage) {
            c.image = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(value));
            c.mask = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(0));
            c.mask(cv::Rect(40, 30, 20, 16)).setTo(255);
        }
        rows_.push_back(c);
        while (rows_.size() > capacity_) rows_.pop_front();
        return rows_.back();
    }
    void evictAll() { rows_.clear(); }
    void evictOldest(std::size_t n)
    {
        for (std::size_t i = 0; i < n && !rows_.empty(); ++i) rows_.pop_front();
    }
    const MonitoringCellCandidate* held(std::uint64_t frameIndex, int objectId) const
    {
        for (const auto& r : rows_)
            if (r.key.frameIndex == frameIndex && r.key.objectId == objectId) return &r;
        return nullptr;
    }

    Rows newestValid(std::size_t maxRows) override
    {
        ++newestCalls;
        Rows out;
        out.ringGeneration = generation;
        out.captureEpoch = captureEpoch;
        std::vector<MonitoringCellCandidate> valid;
        for (auto it = rows_.rbegin(); it != rows_.rend() && valid.size() < maxRows; ++it)
            if (it->metrics.valid) valid.push_back(*it);
        out.rows.assign(valid.rbegin(), valid.rend());
        if (onNewest) onNewest();
        return out;
    }
    Rows find(std::uint64_t frameIndex, int objectId) override
    {
        Rows out;
        out.ringGeneration = generation;
        out.captureEpoch = captureEpoch;
        for (auto it = rows_.rbegin(); it != rows_.rend(); ++it)
            if (it->key.frameIndex == frameIndex && it->key.objectId == objectId) out.rows.push_back(*it);
        if (onFind) onFind();
        return out;
    }

    void releaseShares(std::vector<MonitoringCellCandidate>& rows) override
    {
        ++releaseCalls;
        for (const auto& c : rows) releasedShares += (c.image.empty() ? 0 : 1) + (c.mask.empty() ? 0 : 1);
        MonitoringCellSource::releaseShares(rows);
    }

    std::uint64_t currentCaptureEpoch() const override { return captureEpoch; }

    std::uint64_t generation{0};   // the ring's current run tag
    std::uint64_t captureEpoch{0}; // the ring's current capture epoch
    int newestCalls{0};
    int releaseCalls{0};
    int releasedShares{0};
    std::function<void()> onNewest; // runs inside the source call (a racing beginRun)
    std::function<void()> onFind;

private:
    std::size_t capacity_;
    std::deque<MonitoringCellCandidate> rows_;
};

bool sharesMemory(const cv::Mat& a, const cv::Mat& b)
{
    if (a.empty() || b.empty()) return false;
    return a.datastart < b.dataend && b.datastart < a.dataend;
}

const MonitoringCellInfo* inStrip(const MonitoringCellStore::Listing& l, std::uint64_t frameIndex)
{
    for (const auto& c : l.strip)
        if (c.key.frameIndex == frameIndex) return &c;
    return nullptr;
}
// The result points into the listing: never call it on a temporary (ASan caught one).
const MonitoringCellInfo* inStrip(MonitoringCellStore::Listing&&, std::uint64_t) = delete;

void testCropPolicy()
{
    std::printf("crop policy\n");
    MonitoringCellStore::Policy p;
    MonitoringCellMetrics m;
    m.bboxX = 40;
    m.bboxY = 30;
    m.bboxWidth = 20;
    m.bboxHeight = 16;
    // margin = max(8, 20/2) = 10: 40 x 36 around the box centre (50, 38).
    auto r = MonitoringCellStore::cropRect(kW, kH, m, p);
    MIB_EXPECT(r == cv::Rect(30, 20, 40, 36), "box grown by the margin on each side");
    MIB_EXPECT(r.contains(cv::Point(40, 30)) && r.contains(cv::Point(59, 45)), "the object is inside the crop");
    // Near an edge the window shifts, it does not shrink.
    m.bboxX = 2;
    m.bboxY = 1;
    r = MonitoringCellStore::cropRect(kW, kH, m, p);
    MIB_EXPECT(r.x == 0 && r.y == 0 && r.width == 40 && r.height == 36, "shifted inside, same size");
    // A huge box is capped at crop_max_side around its centre.
    m.bboxX = 0;
    m.bboxY = 0;
    m.bboxWidth = 900;
    m.bboxHeight = 600;
    r = MonitoringCellStore::cropRect(1000, 700, m, p);
    MIB_EXPECT(r.width == p.cropMaxSide && r.height == p.cropMaxSide, "capped at crop_max_side");
    // Bigger than the image: the image.
    r = MonitoringCellStore::cropRect(100, 50, m, p);
    MIB_EXPECT(r == cv::Rect(0, 0, 100, 50), "never larger than the image");
    // Unknown box: centred on the centroid, half the maximum side (clamped).
    MonitoringCellMetrics c;
    c.centroidX = 100;
    c.centroidY = 40;
    r = MonitoringCellStore::cropRect(kW, kH, c, p);
    MIB_EXPECT(r.width == 128 && r.height == kH && r.x == 36, "centroid window");
    // Box outside the image: the centroid (here none either: the image centre).
    MonitoringCellMetrics outside;
    outside.bboxX = 5000;
    outside.bboxY = 5000;
    outside.bboxWidth = 10;
    outside.bboxHeight = 10;
    outside.centroidX = -1;
    r = MonitoringCellStore::cropRect(kW, kH, outside, p);
    MIB_EXPECT(r.x == 36 && r.width == 128, "image centre when nothing usable");
    MIB_EXPECT(MonitoringCellStore::cropRect(0, 0, m, p).empty(), "empty image, empty rect");
}

void testStripCapsAndEviction()
{
    std::printf("strip caps and eviction\n");
    FakeRing ring;
    MonitoringCellStore::Policy p;
    p.stripMaxCells = 4;
    MonitoringCellStore store(ring, p);

    for (std::uint64_t i = 1; i <= 3; ++i) ring.append(i, 0, true, static_cast<std::uint8_t>(i));
    ring.append(4, 0, false, 4); // invalid: never in the strip
    store.refreshStrip();
    auto l = store.listing();
    MIB_REQUIRE(l.strip.size() == 3, "three valid cells in the strip");
    MIB_EXPECT(l.strip.front().key.frameIndex == 1 && l.strip.back().key.frameIndex == 3, "oldest -> newest");
    MIB_EXPECT(!inStrip(l, 4), "invalid rows are not strip cells");
    MIB_EXPECT(l.strip[0].hasImage && l.strip[0].hasMask && l.strip[0].width == 40 && l.strip[0].height == 36,
               "a crop around the object, with its mask");
    MIB_EXPECT(l.strip[0].bytes == 2u * 40 * 36, "bytes = image + mask");
    MIB_EXPECT(l.stats.stripBytes == 3u * 2 * 40 * 36, "byte accounting");
    const auto rev = l.revision;

    // Owned copies: identical pixels, no alias of the ring's buffers.
    cv::Mat image, mask;
    MIB_REQUIRE(store.image({0, 2, 0}, false, image), "strip image fetchable");
    MIB_REQUIRE(store.image({0, 2, 0}, true, mask), "strip mask fetchable");
    MIB_EXPECT(cv::countNonZero(image != 2) == 0, "the crop holds the row's pixels");
    MIB_EXPECT(cv::countNonZero(mask) == 20 * 16, "the mask crop holds the object's mask");
    MIB_EXPECT(!sharesMemory(image, ring.held(2, 0)->image), "the copy does not alias the ring");

    // Unchanged ring: no new copy, same revision.
    store.refreshStrip();
    MIB_EXPECT(store.listing().revision == rev, "an unchanged ring changes nothing");
    MIB_EXPECT(store.listing().stats.stripAdmitted == 3, "no duplicate admission");

    // Three more valid cells: the count cap (4) evicts the two oldest.
    for (std::uint64_t i = 5; i <= 7; ++i) ring.append(i, 0, true, static_cast<std::uint8_t>(i));
    store.refreshStrip();
    l = store.listing();
    MIB_REQUIRE(l.strip.size() == 4, "count cap");
    MIB_EXPECT(l.strip.front().key.frameIndex == 3 && l.strip.back().key.frameIndex == 7, "oldest evicted first");
    MIB_EXPECT(l.stats.stripEvicted == 2, "evictions are counted");
    MIB_EXPECT(!store.image({0, 1, 0}, false, image), "an evicted strip cell is gone");

    // Byte cap: room for two cells only.
    FakeRing ring2;
    MonitoringCellStore::Policy small;
    small.stripMaxBytes = 2u * 2 * 40 * 36;
    MonitoringCellStore tight(ring2, small);
    for (std::uint64_t i = 1; i <= 5; ++i) ring2.append(i, 0, true, 9);
    tight.refreshStrip();
    l = tight.listing();
    MIB_EXPECT(l.strip.size() == 2 && l.stats.stripBytes <= small.stripMaxBytes, "byte cap holds");
    MIB_EXPECT(l.strip.front().key.frameIndex == 4, "the newest that fit are kept");
    MIB_EXPECT(l.stats.stripEvicted == 3, "byte evictions are counted");

    // Strip copies are bounded by the cap per refresh, never the ring's size.
    FakeRing big;
    MonitoringCellStore bounded(big);
    for (std::uint64_t i = 1; i <= 500; ++i) big.append(i, 0, true, 1);
    bounded.refreshStrip();
    MIB_EXPECT(bounded.listing().stats.stripAdmitted == MonitoringCellStore::kStripMaxCells,
               "at most strip_max_cells copies per refresh");
}

void testStripFollowsTheContext()
{
    std::printf("strip follows the ring's context\n");
    FakeRing ring;
    MonitoringCellStore store(ring);
    ring.append(1, 0, true, 1); // outside a run
    ring.append(2, 0, true, 2);
    store.refreshStrip();
    MIB_EXPECT(store.listing().strip.size() == 2 && store.listing().ringGeneration == 0, "outside a run");

    ring.generation = 7; // run 7 starts tagging rows
    store.beginRun(7);
    store.refreshStrip();
    MIB_EXPECT(store.listing().strip.empty(), "pre-run cells never enter the run's strip");
    ring.append(3, 0, true, 3);
    store.refreshStrip();
    auto l = store.listing();
    MIB_EXPECT(l.strip.size() == 1 && l.strip[0].key.startGeneration == 7 && l.ringGeneration == 7,
               "the run's cells");

    ring.generation = 0; // the run stopped accumulating
    ring.append(4, 0, true, 4);
    store.refreshStrip();
    l = store.listing();
    MIB_EXPECT(!inStrip(l, 3) && inStrip(l, 4), "after the run, the run's cells leave the strip");
    bool allOutside = !l.strip.empty();
    for (const auto& c : l.strip) allOutside = allOutside && c.key.startGeneration == 0;
    MIB_EXPECT(allOutside, "the strip holds the newest cells outside a run (here 1, 2 and 4)");
    MIB_EXPECT(l.strip.back().key.frameIndex == 4, "oldest -> newest");
    MIB_EXPECT(l.scopeGeneration == 7, "pins still belong to run 7 until the next start");

    // A run starting during a refresh: the stale batch is dropped.
    ring.generation = 8;
    ring.append(5, 0, true, 5);
    ring.onNewest = [&] { store.beginRun(8); };
    store.refreshStrip();
    ring.onNewest = nullptr;
    MIB_EXPECT(store.listing().strip.empty(), "a batch read before beginRun is not admitted after it");
}

void testPinSurvivesEviction()
{
    std::printf("pin survives eviction\n");
    FakeRing ring(8); // a small ring: eviction is easy to provoke
    MonitoringCellStore::Policy p;
    p.stripMaxCells = 2;
    MonitoringCellStore store(ring, p);
    ring.generation = 3;
    store.beginRun(3);
    ring.append(10, 1, true, 77);
    const cv::Mat ringImage = ring.held(10, 1)->image;

    // Pinned from the ring (the strip never saw it).
    auto r = store.pin({3, 10, 1});
    MIB_REQUIRE(r.outcome == Outcome::Pinned, r.message);
    MIB_EXPECT(r.cell && r.cell->hasImage && r.cell->hasMask && r.cell->key == (MonitoringCellKey{3, 10, 1}),
               "the reply carries the pin");
    cv::Mat pinned;
    MIB_REQUIRE(store.image({3, 10, 1}, false, pinned), "pinned image");
    MIB_EXPECT(!sharesMemory(pinned, ringImage), "an owned copy, not a view of the ring");

    // The ring and the strip turn over completely.
    for (std::uint64_t i = 11; i < 40; ++i) {
        ring.append(i, 0, true, 1);
        store.refreshStrip();
    }
    MIB_EXPECT(!ring.held(10, 1), "the ring has evicted the row");
    MIB_EXPECT(store.listing().stats.stripEvicted > 0, "the strip has evicted cells");
    cv::Mat after, mask;
    MIB_REQUIRE(store.image({3, 10, 1}, false, after), "the pin survives ring and strip eviction");
    MIB_REQUIRE(store.image({3, 10, 1}, true, mask), "with its mask");
    MIB_EXPECT(after.size() == pinned.size() && cv::countNonZero(after != 77) == 0, "the pixels are the row's");
    MIB_EXPECT(store.listing().pins.size() == 1, "still pinned");

    // A strip cell pinned, then evicted from the strip: the pin keeps it.
    ring.append(50, 0, true, 50);
    store.refreshStrip();
    MIB_REQUIRE(store.pin({3, 50, 0}).outcome == Outcome::Pinned, "pin a strip cell");
    for (std::uint64_t i = 51; i < 60; ++i) ring.append(i, 0, true, 1);
    store.refreshStrip();
    const auto afterTrim = store.listing();
    MIB_EXPECT(!inStrip(afterTrim, 50), "evicted from the strip");
    MIB_EXPECT(store.image({3, 50, 0}, false, after) && cv::countNonZero(after != 50) == 0,
               "the pinned strip cell survives the strip's eviction");

    // Idempotent.
    r = store.pin({3, 50, 0});
    MIB_EXPECT(r.outcome == Outcome::AlreadyPinned && r.cell, "AlreadyPinned returns the pin");
    MIB_EXPECT(store.listing().pins.size() == 2, "no duplicate");
}

void testPinCaps()
{
    std::printf("pin caps\n");
    FakeRing ring;
    MonitoringCellStore::Policy p;
    p.maxPins = 3;
    MonitoringCellStore store(ring, p);
    for (std::uint64_t i = 1; i <= 5; ++i) ring.append(i, 0, true, 1);
    for (std::uint64_t i = 1; i <= 3; ++i)
        MIB_REQUIRE(store.pin({0, i, 0}).outcome == Outcome::Pinned, "pin within the cap");
    auto r = store.pin({0, 4, 0});
    MIB_EXPECT(r.outcome == Outcome::LimitReached && !r.cell, "the count cap refuses");
    MIB_EXPECT(r.message.find("unpin one first") != std::string::npos, r.message);
    const auto pins = store.listing().pins;
    MIB_EXPECT(pins.size() == 3 && pins.front().key.frameIndex == 1, "an older pin is never evicted");
    MIB_EXPECT(store.unpin({0, 1, 0}), "unpin");
    MIB_EXPECT(!store.unpin({0, 1, 0}), "unpin twice: not pinned");
    MIB_EXPECT(store.pin({0, 4, 0}).outcome == Outcome::Pinned, "room again after an unpin");

    // Byte cap.
    FakeRing ring2;
    MonitoringCellStore::Policy bytes;
    bytes.pinMaxBytes = 2u * 40 * 36 + 10; // one cell (image + mask)
    MonitoringCellStore tight(ring2, bytes);
    ring2.append(1, 0, true, 1);
    ring2.append(2, 0, true, 1);
    MIB_REQUIRE(tight.pin({0, 1, 0}).outcome == Outcome::Pinned, "first fits");
    r = tight.pin({0, 2, 0});
    MIB_EXPECT(r.outcome == Outcome::LimitReached && r.message.find("bytes") != std::string::npos, r.message);
    MIB_EXPECT(tight.listing().stats.pinBytes <= bytes.pinMaxBytes, "byte cap holds");
}

void testRunRules()
{
    std::printf("run rules\n");
    FakeRing ring;
    MonitoringCellStore store(ring);
    ring.append(1, 0, true, 1); // outside a run (gen 0)
    ring.generation = 4;
    store.beginRun(4);
    ring.append(2, 0, true, 2);

    MIB_EXPECT(store.pin({4, 2, 0}).outcome == Outcome::Pinned, "the run's row");
    MIB_EXPECT(store.pin({0, 1, 0}).outcome == Outcome::Pinned, "a row outside a run, as run 0");
    auto r = store.pin({4, 1, 0});
    MIB_EXPECT(r.outcome == Outcome::RunMismatch && r.message.find("outside a run") != std::string::npos,
               "a row of no run is not run 4's: " + r.message);
    r = store.pin({3, 2, 0});
    MIB_EXPECT(r.outcome == Outcome::RunMismatch && r.message.find("not the latest run") != std::string::npos,
               "a stale generation: " + r.message);
    r = store.pin({5, 2, 0});
    MIB_EXPECT(r.outcome == Outcome::RunMismatch, "a future generation: " + r.message);

    // The next run drops every pin; the old run's pins are refused from then on.
    ring.generation = 5;
    store.beginRun(5);
    auto l = store.listing();
    MIB_EXPECT(l.pins.empty() && l.strip.empty() && l.scopeGeneration == 5, "dropped at the next run start");
    cv::Mat image;
    MIB_EXPECT(!store.image({4, 2, 0}, false, image), "no image of a dropped pin");
    r = store.pin({4, 2, 0});
    MIB_EXPECT(r.outcome == Outcome::RunMismatch, "a pin for the previous run is refused: " + r.message);
    ring.append(3, 0, true, 3);
    MIB_EXPECT(store.pin({5, 3, 0}).outcome == Outcome::Pinned, "the new run's rows pin");

    // A run starting while the pin is taken: refused, nothing admitted.
    ring.append(4, 0, true, 4);
    ring.onFind = [&] { store.beginRun(6); };
    r = store.pin({5, 4, 0});
    ring.onFind = nullptr;
    MIB_EXPECT(r.outcome == Outcome::RunMismatch && store.listing().pins.empty(),
               "a pin racing a run start is refused: " + r.message);
}

void testNoImage()
{
    std::printf("no image, never a substitute\n");
    FakeRing ring;
    MonitoringCellStore store(ring);
    ring.generation = 2;
    store.beginRun(2);
    ring.append(1, 0, true, 1);
    // The monitoring snapshot served the row, then the ring evicted it.
    MonitoringCellMetrics served = ring.held(1, 0)->metrics;
    served.area = 321.5;
    store.noteRows({{{2, 1, 0}, served}});
    ring.evictAll();
    auto r = store.pin({2, 1, 0});
    MIB_REQUIRE(r.outcome == Outcome::PinnedWithoutImage && r.cell, "evicted: pinned with its measurements");
    MIB_EXPECT(!r.cell->hasImage && !r.cell->hasMask && r.cell->bytes == 0, "no pixels");
    MIB_EXPECT(r.cell->metrics.area == 321.5, "the measurements the snapshot reported");
    MIB_EXPECT(r.message.find("left the monitoring buffer") != std::string::npos &&
                   r.message == r.cell->noImageReason,
               "a plain reason: " + r.message);
    cv::Mat image;
    MIB_EXPECT(!store.image({2, 1, 0}, false, image), "no image is served for it");

    // Unknown to the backend: refused, nothing invented.
    r = store.pin({2, 999, 0});
    MIB_EXPECT(r.outcome == Outcome::NotFound && !r.cell, "NotFound: " + r.message);
    MIB_EXPECT(r.message.find("frame 999 object 0") != std::string::npos, r.message);

    // A results-only row (the PL sends no pixels): metadata only, said so.
    ring.append(7, 2, true, 0, /*withImage=*/false);
    store.refreshStrip();
    const auto resultsOnly = store.listing();
    const auto* strip = inStrip(resultsOnly, 7);
    MIB_REQUIRE(strip, "a results-only cell is listed in the strip");
    MIB_EXPECT(!strip->hasImage && strip->noImageReason.find("without images") != std::string::npos,
               strip->noImageReason);
    r = store.pin({2, 7, 2});
    MIB_EXPECT(r.outcome == Outcome::PinnedWithoutImage, "results-only pin: " + r.message);
}

void testClear()
{
    std::printf("unpin, clear pins, clear strip\n");
    FakeRing ring;
    MonitoringCellStore store(ring);
    for (std::uint64_t i = 1; i <= 3; ++i) ring.append(i, 0, true, 1);
    store.refreshStrip();
    MIB_REQUIRE(store.pin({0, 1, 0}).outcome == Outcome::Pinned, "pin");
    MIB_REQUIRE(store.pin({0, 2, 0}).outcome == Outcome::Pinned, "pin");
    store.clearStrip(); // monitoring_clear
    auto l = store.listing();
    MIB_EXPECT(l.strip.empty() && l.pins.size() == 2 && l.stats.indexedRows == 0, "the strip clears, pins stay");
    MIB_EXPECT(store.clearPins() == 2 && store.listing().pins.empty(), "clear pins");
    MIB_EXPECT(store.clearPins() == 0, "clear is idempotent");
    MIB_EXPECT(store.listing().stats.pinBytes == 0, "bytes released");
}

void testRowIndexBound()
{
    std::printf("row index bound\n");
    FakeRing ring;
    MonitoringCellStore::Policy p;
    p.rowIndexMax = 10;
    MonitoringCellStore store(ring, p);
    std::vector<std::pair<MonitoringCellKey, MonitoringCellMetrics>> rows;
    for (std::uint64_t i = 0; i < 25; ++i) rows.push_back({{0, i, 0}, MonitoringCellMetrics{}});
    store.noteRows(rows);
    store.noteRows(rows); // duplicates do not grow it
    MIB_EXPECT(store.listing().stats.indexedRows == 10, "bounded FIFO");
    MIB_EXPECT(store.pin({0, 24, 0}).outcome == Outcome::PinnedWithoutImage, "newest rows are kept");
    MIB_EXPECT(store.pin({0, 0, 0}).outcome == Outcome::NotFound, "oldest rows fall out");
}

// Only the ring's own reference is left: no share of a ring image stays with the store.
int refs(const cv::Mat& m) { return m.u ? m.u->refcount : 0; }

void testSharesGoBack()
{
    std::printf("shares go back, private copies out\n");
    FakeRing ring;
    MonitoringCellStore store(ring);
    for (std::uint64_t i = 1; i <= 3; ++i) ring.append(i, 0, true, static_cast<std::uint8_t>(10 * i));
    store.refreshStrip();
    MIB_EXPECT(ring.releaseCalls == 1 && ring.releasedShares == 6, "the strip refresh returned 3 images + 3 masks");
    for (std::uint64_t i = 1; i <= 3; ++i)
        MIB_EXPECT(refs(ring.held(i, 0)->image) == 1 && refs(ring.held(i, 0)->mask) == 1,
                   "the store keeps no share of row " + std::to_string(i));
    // Unchanged ring: the shares of rows already held go back too.
    store.refreshStrip();
    MIB_EXPECT(ring.releaseCalls == 2 && ring.releasedShares == 12, "every refresh returns its shares");

    // A pin from the ring (an invalid row is never a strip cell) and a refused one.
    ring.append(9, 1, false, 90);
    auto r = store.pin({0, 9, 1});
    MIB_REQUIRE(r.outcome == Outcome::Pinned, r.message);
    MIB_EXPECT(ring.releaseCalls == 3 && ring.releasedShares == 14, "the pin returned the row's image + mask");
    MIB_EXPECT(refs(ring.held(9, 1)->image) == 1, "the pin keeps an owned crop, not a share");
    ring.append(10, 0, false, 1);
    store.beginRun(4);
    r = store.pin({4, 10, 0}); // the row is tagged 0, not 4
    MIB_EXPECT(r.outcome == Outcome::RunMismatch && ring.releaseCalls == 4, "a refused pin returns its shares");

    // image(): a private copy each time; writing to it changes nothing in the store.
    ring.generation = 4;
    ring.append(11, 0, true, 110);
    MIB_REQUIRE(store.pin({4, 11, 0}).outcome == Outcome::Pinned, "pin");
    cv::Mat a, b;
    MIB_REQUIRE(store.image({4, 11, 0}, false, a) && store.image({4, 11, 0}, false, b), "image");
    MIB_EXPECT(!sharesMemory(a, b) && refs(a) == 1 && refs(b) == 1, "two calls, two private copies");
    a.setTo(0);
    MIB_REQUIRE(store.image({4, 11, 0}, false, b), "image again");
    MIB_EXPECT(cv::countNonZero(b != 110) == 0, "the store's copy is untouched by the caller");
}

void testCaptureChange()
{
    std::printf("capture change before the store hears of it: identities recur, cells never cross\n");
    FakeRing ring;
    ring.captureEpoch = 7; // capture A: frames 1..3, pixel value 10 * frame
    MonitoringCellStore store(ring);
    for (std::uint64_t i = 1; i <= 3; ++i) ring.append(i, 0, true, static_cast<std::uint8_t>(10 * i));
    store.refreshStrip();
    MIB_REQUIRE(store.pin({0, 1, 0, 7}).outcome == Outcome::Pinned, "pin A's frame 1");
    MIB_REQUIRE(store.listing().strip.size() == 3, "A's strip");

    // Capture B (the append path's backstop moved the epoch: no notification): the same indices,
    // other pixels. The store still holds A's strip and A's pin.
    ring.evictAll(); // the switch keeps nothing of A in this fake; the real ring may keep some
    ring.captureEpoch = 8;
    for (std::uint64_t i = 1; i <= 3; ++i) ring.append(i, 0, true, static_cast<std::uint8_t>(100 + i));
    cv::Mat px;
    auto read = store.image({0, 2, 0, 7}, false, px);
    MIB_EXPECT(!read && read.captureMismatch && px.empty(), "A's strip image is refused at once: " + read.message);
    MIB_EXPECT(read.message.find("capture 7") != std::string::npos &&
                   read.message.find("current capture is 8") != std::string::npos,
               read.message);
    read = store.image({0, 1, 0, 7}, true, px);
    MIB_EXPECT(!read && read.captureMismatch && px.empty(), "A's pinned mask is refused too: " + read.message);
    auto l = store.listing();
    MIB_EXPECT(l.ringCaptureEpoch == 8 && l.strip.empty() && l.pins.empty(), "nothing of A is listed");
    auto r = store.pin({0, 1, 0, 7});
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch && !r.cell,
               "A's pinned identity is CaptureMismatch, not AlreadyPinned: " + r.message);
    r = store.pin({0, 3, 0, 7});
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch && !r.cell, "an earlier capture is refused: " + r.message);

    // The first pin of B applies the change: A's copies go, B's pin gets B's pixels.
    r = store.pin({0, 2, 0, 8});
    MIB_REQUIRE(r.outcome == Outcome::Pinned && r.cell && r.cell->key.captureEpoch == 8, r.message);
    MIB_REQUIRE(store.image({0, 2, 0, 8}, false, px), "B's pin has an image");
    MIB_EXPECT(cv::countNonZero(px != 102) == 0, "B's frame 2 pixels, not A's (20)");
    l = store.listing();
    MIB_EXPECT(l.pins.size() == 1 && l.stats.pins == 1 && l.stats.stripCells == 0, "A's pin and strip are gone");
    MIB_EXPECT(l.lastCaptureChange && l.lastCaptureChange->captureEpoch == 8 &&
                   l.lastCaptureChange->pinsDropped == 1 && l.lastCaptureChange->stripDropped == 3,
               "the change is reported");
    const auto u = store.unpin({0, 1, 0, 7});
    MIB_EXPECT(!u && u.captureMismatch && u.message.find("dropped") != std::string::npos, u.message);

    // The strip moves to B with B's pixels.
    store.refreshStrip();
    l = store.listing();
    MIB_EXPECT(l.ringCaptureEpoch == 8 && l.strip.size() == 3, "the strip is B's");
    for (const auto& c : l.strip) {
        MIB_EXPECT(c.key.captureEpoch == 8, "only B's cells");
        MIB_REQUIRE(store.image(c.key, false, px), "strip image");
        MIB_EXPECT(cv::countNonZero(px != static_cast<int>(100 + c.key.frameIndex)) == 0, "B's pixels");
    }
    MIB_REQUIRE(store.pin({0, 1, 0, 8}).outcome == Outcome::Pinned, "B's frame 1 is a new pin");
    MIB_EXPECT(store.listing().pins.size() == 2, "B2, B1");
}

void testCaptureNotified()
{
    std::printf("capture notifications: dropped at the moment of the change\n");
    FakeRing ring;
    ring.captureEpoch = 1;
    MonitoringCellStore store(ring);
    for (std::uint64_t i = 1; i <= 4; ++i) ring.append(i, 0, true, 40);
    store.refreshStrip();
    MIB_REQUIRE(store.pin({0, 4, 0, 1}).outcome == Outcome::Pinned, "pin");
    store.noteRows({{{0, 9, 0, 1}, MonitoringCellMetrics{}}});
    MIB_REQUIRE(store.listing().stats.indexedRows > 0 && store.memoryStats().currentBytes > 0, "held");
    const auto revision = store.listing().revision;

    // ProcessingService moves the epoch, then notifies (no refresh, no read in between).
    ring.captureEpoch = 2;
    store.captureChanged(2, "the camera stopped");
    auto l = store.listing();
    MIB_EXPECT(l.strip.empty() && l.pins.empty() && l.stats.stripCells == 0 && l.stats.pins == 0 &&
                   l.stats.indexedRows == 0 && store.memoryStats().currentBytes == 0,
               "strip, pins and index dropped at once");
    MIB_EXPECT(l.revision > revision && l.ringCaptureEpoch == 2, "the listing moved on");
    MIB_REQUIRE(l.lastCaptureChange, "the change is reported");
    MIB_EXPECT(l.lastCaptureChange->captureEpoch == 2 && l.lastCaptureChange->reason == "the camera stopped" &&
                   l.lastCaptureChange->pinsDropped == 1 && l.lastCaptureChange->stripDropped == 4,
               "with its reason and counts");
    cv::Mat px;
    const auto read = store.image({0, 4, 0, 1}, false, px);
    MIB_EXPECT(!read && read.captureMismatch && read.message.find("(the camera stopped)") != std::string::npos,
               "the refusal names the change: " + read.message);
    const auto r = store.pin({0, 3, 0, 1});
    MIB_EXPECT(r.outcome == Outcome::CaptureMismatch && r.message.find("the camera stopped") != std::string::npos,
               r.message);

    // Late and repeated notifications change nothing; a newer capture's cells are kept.
    ring.captureEpoch = 3;
    ring.evictAll();
    ring.append(20, 0, true, 60);
    store.refreshStrip(); // the backstop path: adopts 3, admits capture 3's cell
    MIB_REQUIRE(store.listing().strip.size() == 1, "capture 3's cell");
    store.captureChanged(3, "late notification of capture 3");
    store.captureChanged(2, "a notification out of order");
    l = store.listing();
    MIB_EXPECT(l.strip.size() == 1 && l.lastCaptureChange && l.lastCaptureChange->captureEpoch == 3 &&
                   l.lastCaptureChange->reason != "a notification out of order",
               "nothing dropped, the latest change kept");

    // A refresh under way when the capture changes admits nothing of the old capture.
    ring.append(21, 0, true, 61);
    ring.onNewest = [&] {
        ring.captureEpoch = 4;
        store.captureChanged(4, "the processing ROI changed");
    };
    store.refreshStrip();
    ring.onNewest = nullptr;
    l = store.listing();
    MIB_EXPECT(l.strip.empty() && l.stats.stripCells == 0, "the stale batch is discarded and capture 3 dropped");
    MIB_EXPECT(l.lastCaptureChange && l.lastCaptureChange->reason == "the processing ROI changed", "reason");
}

void testGeometryOrigin()
{
    std::printf("geometry origin: full-frame boxes into image coordinates\n");
    backend::services::FilterResult v;
    v.bboxX = 140;
    v.bboxY = 50;
    v.bboxWidth = 20;
    v.bboxHeight = 16;
    v.centroidX = 150;
    v.centroidY = 58;
    const auto m = backend::services::monitoringCellMetrics(v, 0, {100, 40});
    MIB_EXPECT(m.bboxX == 40 && m.bboxY == 10 && m.bboxWidth == 20 && m.bboxHeight == 16, "box in ROI coordinates");
    MIB_EXPECT(m.centroidX == 50 && m.centroidY == 18, "centroid in ROI coordinates");
    const auto inline_ = backend::services::monitoringCellMetrics(v, 0);
    MIB_EXPECT(inline_.bboxX == 140 && inline_.centroidY == 58, "origin (0, 0) leaves inline rows as measured");
}

void testMemoryStats()
{
    FakeRing ring;
    MonitoringCellStore store(ring);
    ring.append(1, 0, true, 1);
    store.refreshStrip();
    const auto m = store.memoryStats();
    MIB_EXPECT(m.name == "monitoring.cellStore" && m.currentBytes == 2u * 40 * 36 && m.currentCount == 1,
               "memory owner reports the copies");
    MIB_EXPECT(m.capacityBytes == MonitoringCellStore::kStripMaxBytes + MonitoringCellStore::kPinMaxBytes,
               "declared budget");
}

} // namespace

int main()
{
    testCropPolicy();
    testStripCapsAndEviction();
    testStripFollowsTheContext();
    testPinSurvivesEviction();
    testPinCaps();
    testRunRules();
    testNoImage();
    testClear();
    testRowIndexBound();
    testSharesGoBack();
    testCaptureChange();
    testCaptureNotified();
    testGeometryOrigin();
    testMemoryStats();
    std::printf("monitoring_cell_store_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
