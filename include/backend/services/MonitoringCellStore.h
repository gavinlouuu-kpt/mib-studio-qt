#pragma once
// MonitoringCellStore: bounded owned copies of live detected cells for the
// React Live view (#716 A6a, bridge ABI 41), the backend counterpart of what
// the Qt Monitoring tab shows from its own rolling buffer.
//
// Two collections, both owned copies that survive the monitoring ring:
//   - the strip: the most recently detected valid cells, a crop around each
//     object plus the same crop of the mask. Refreshed from the ring on read
//     (refreshStrip), oldest evicted first when the count or byte cap is
//     exceeded; every eviction is counted. The strip follows the ring's
//     current context: rows of the active run while one runs, rows detected
//     outside a run otherwise, and only of the current capture (a row of
//     another context is dropped at the next refresh).
//   - pins: rows the operator asked to keep, by identity (start generation,
//     frame index, object id, capture epoch). A pin is refused for a run other
//     than the latest one started (and run 0, "outside a run") and for any
//     capture but the current one (frame indices restart when the capture
//     changes, so an older capture's identity is never resolved against the
//     new one); all pins are dropped when the next run starts (beginRun).
//     The count and the bytes are capped and a full store refuses
//     (LimitReached): a pin is never evicted to make room for another.
// A row whose image is gone is never given a substitute: a pin of a row the
// ring no longer holds keeps the row's measurements (from the bounded index
// of rows the monitoring snapshot served) with a plain reason, and a row the
// backend knows nothing about is NotFound.
//
// Threading: no method runs on the acquisition/processing path. The source
// reads the ring under its lock and copies only scalars and cv::Mat headers
// (refcount shares of the frozen images); crops and clones happen outside
// every lock, and the shares then go back to the source, which drops them
// under the ring's lock again (releaseShares). The store's own mutex guards
// its collections and is never held while calling the source. Owned copies
// are immutable once admitted and never shared outside the store: image()
// hands out a private copy taken under the store's mutex. Both rules order
// every free of a pixel buffer after its readers by a lock, the only
// synchronisation ThreadSanitizer can see (cv::Mat's reference count lives in
// the uninstrumented OpenCV library).

#include "backend/diagnostics/MemoryBudget.h"
#include "backend/processing/ProcessingTypes.h"

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backend::services {

class ProcessingService;

// Identity of one monitoring row (one detected object), stable across
// snapshots: the start generation of the run active when the processing path
// appended the row (0 = outside a run), its frame index, its object id, and
// the capture epoch of its frame (ProcessingService::monitoringCaptureEpoch:
// frame indices restart when the capture changes, the epoch never repeats).
struct MonitoringCellKey {
    std::uint64_t startGeneration{0};
    std::uint64_t frameIndex{0};
    int objectId{-1};
    std::uint64_t captureEpoch{0};
    bool operator==(const MonitoringCellKey& o) const
    {
        return startGeneration == o.startGeneration && frameIndex == o.frameIndex && objectId == o.objectId &&
               captureEpoch == o.captureEpoch;
    }
    bool operator!=(const MonitoringCellKey& o) const { return !(*this == o); }
};

struct MonitoringCellKeyHash {
    std::size_t operator()(const MonitoringCellKey& k) const noexcept
    {
        std::uint64_t h = k.startGeneration * 0x9E3779B97F4A7C15ull;
        h ^= k.frameIndex + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.objectId)) + (h << 6) + (h >> 2);
        h ^= k.captureEpoch + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

// The row's measurements, as the monitoring snapshot reports them (metadata only).
// Geometry (centroid, bounding box) is in the coordinates of the row's image,
// the processing ROI, whichever realtime mode measured it.
struct MonitoringCellMetrics {
    std::uint64_t timestampNs{0};
    bool valid{false};
    bool targetGroup{false};
    int objectCount{0};
    int trackId{-1};
    double centroidX{0.0};
    double centroidY{0.0};
    double bboxX{0.0};
    double bboxY{0.0};
    double bboxWidth{0.0};
    double bboxHeight{0.0};
    double area{0.0}; // px^2
    double deformability{0.0};
    double areaRatio{0.0};
    double ringRatio{0.0};
    double youngsModulus{0.0};
    double pixelToMicron{0.0}; // analysis-time factor; 0 = unknown
};

// `geometryOrigin`: where the row's image sits in the coordinate space of `v`'s
// geometry (ProcessingService::MonitoringRowTag); subtracted from the centroid
// and the bounding box, so the result is in image (ROI) coordinates.
MonitoringCellMetrics monitoringCellMetrics(const FilterResult& v, std::uint64_t timestampNs,
                                            cv::Point geometryOrigin = {});

// A ring row as the source hands it over: identity, measurements and refcount
// shares of its frozen images (headers only, taken under the ring's lock).
struct MonitoringCellCandidate {
    MonitoringCellKey key;
    MonitoringCellMetrics metrics;
    cv::Mat image; // read-only share; empty when the row has no image (PL rows); see releaseShares
    cv::Mat mask;  // read-only share of the processed mask; may be empty
};

// Where the store reads rows from: the monitoring ring (ProcessingService) in
// production, a fake in tests. Both calls happen off the hot path; an
// implementation holds its lock only while copying headers.
class MonitoringCellSource {
public:
    struct Rows {
        std::uint64_t ringGeneration{0}; // the run tag rows get now (the active run, or 0)
        std::uint64_t captureEpoch{0};   // the capture epoch rows get now
        std::vector<MonitoringCellCandidate> rows;
    };
    virtual ~MonitoringCellSource() = default;
    // The newest valid rows, at most maxRows, oldest -> newest.
    virtual Rows newestValid(std::size_t maxRows) = 0;
    // Every held row (valid or not) with this frame index and object id, any
    // run or capture, newest first.
    virtual Rows find(std::uint64_t frameIndex, int objectId) = 0;
    // The store calls this once it has cropped what it needs: drops the rows' image and mask
    // shares (keys and metrics stay). The production source drops them under the ring's lock
    // (ProcessingService::releaseMonitoringShares); the default just drops them.
    virtual void releaseShares(std::vector<MonitoringCellCandidate>& rows);
};

// The production source: ProcessingService::visitMonitoringNewest over the monitoring ring.
std::unique_ptr<MonitoringCellSource> makeProcessingCellSource(const ProcessingService& processing);

// What a listing reports per cell; no pixels.
struct MonitoringCellInfo {
    MonitoringCellKey key;
    MonitoringCellMetrics metrics;
    bool hasImage{false};
    bool hasMask{false};
    int width{0};  // crop size in pixels (image and mask alike)
    int height{0};
    int cropX{0};  // crop origin in the row's source image
    int cropY{0};
    int sourceWidth{0}; // the row's source image (the processing ROI)
    int sourceHeight{0};
    std::uint64_t bytes{0};    // pixels this copy keeps alive
    std::string noImageReason; // plain reason, set when !hasImage
};

class MonitoringCellStore {
public:
    // Contract-pinned caps (bridge-contract.json monitoring_cells, static_asserts in shim.cpp).
    static constexpr std::size_t kStripMaxCells = 24;
    static constexpr std::uint64_t kStripMaxBytes = 4ull * 1024 * 1024;
    static constexpr std::size_t kMaxPins = 16;
    static constexpr std::uint64_t kPinMaxBytes = 4ull * 1024 * 1024;
    static constexpr int kCropMaxSide = 256;
    static constexpr int kCropMinMargin = 8;
    static constexpr std::size_t kRowIndexMax = 4096;

    struct Policy {
        std::size_t stripMaxCells{kStripMaxCells};
        std::uint64_t stripMaxBytes{kStripMaxBytes};
        std::size_t maxPins{kMaxPins};
        std::uint64_t pinMaxBytes{kPinMaxBytes};
        int cropMaxSide{kCropMaxSide};
        int cropMinMargin{kCropMinMargin};
        std::size_t rowIndexMax{kRowIndexMax};
    };

    // Values pinned by bridge-contract.json monitoring_cell_pin_outcomes.
    enum class PinOutcome : std::uint32_t {
        Pinned = 0,             // pinned with its image (and mask when the row has one)
        PinnedWithoutImage = 1, // pinned with the row's measurements only; `message` says why
        AlreadyPinned = 2,      // no change; the existing pin is returned
        RunMismatch = 3,        // another run's (or context's) row: refused, nothing changed
        NotFound = 4,           // the backend holds nothing for that identity: refused
        LimitReached = 5,       // the pin count or byte cap is full: refused, unpin first
        CaptureMismatch = 6,    // the identity names an earlier capture: refused, nothing changed
    };
    struct PinResult {
        PinOutcome outcome{PinOutcome::NotFound};
        std::string message;
        std::optional<MonitoringCellInfo> cell; // the pin (Pinned, PinnedWithoutImage, AlreadyPinned)
    };

    struct Stats {
        std::size_t stripCells{0};
        std::uint64_t stripBytes{0};
        std::size_t pins{0};
        std::uint64_t pinBytes{0};
        std::size_t indexedRows{0};
        std::uint64_t peakBytes{0};
        std::uint64_t stripAdmitted{0}; // copies taken into the strip
        std::uint64_t stripEvicted{0};  // strip entries evicted by a cap or a context change
        std::uint64_t refreshes{0};
        std::uint64_t lastRefreshUs{0}; // wall time of the last refresh (source read + crops)
        std::uint64_t lastSourceUs{0};  // of which: the source read (the ring's lock is held inside)
    };

    struct Listing {
        std::uint64_t scopeGeneration{0}; // latest run started (0 before the first); pins belong to it
        std::uint64_t ringGeneration{0};  // the strip's context at the last refresh: run tag
        std::uint64_t ringCaptureEpoch{0}; // ... and capture epoch
        std::uint64_t revision{0};        // bumps whenever the strip or the pins change
        std::vector<MonitoringCellInfo> strip; // oldest -> newest
        std::vector<MonitoringCellInfo> pins;  // in pin order
        Stats stats;
        Policy policy;
    };

    explicit MonitoringCellStore(MonitoringCellSource& source);
    MonitoringCellStore(MonitoringCellSource& source, Policy policy);
    MonitoringCellStore(const MonitoringCellStore&) = delete;
    MonitoringCellStore& operator=(const MonitoringCellStore&) = delete;

    // A run started (ExperimentCoordinator, at Active): the strip, the pins and
    // the row index are dropped, and pins now belong to this run.
    void beginRun(std::uint64_t startGeneration);
    std::uint64_t scopeGeneration() const;

    // Bring the strip up to date with the ring (bounded: at most stripMaxCells
    // new crops). Called on read by the bridge, never from the hot path.
    void refreshStrip();
    // monitoring_clear: the ring was cleared, so is the strip (and the index).
    void clearStrip();

    // `key.startGeneration` is the run the caller expects the row to belong to
    // (0 = outside a run) and `key.captureEpoch` the capture it was detected in.
    PinResult pin(const MonitoringCellKey& key);
    bool unpin(const MonitoringCellKey& key);
    std::size_t clearPins();

    // Rows the monitoring snapshot served (metadata only): what a pin of an
    // evicted row can still report. Bounded FIFO of kRowIndexMax rows.
    void noteRows(const std::vector<std::pair<MonitoringCellKey, MonitoringCellMetrics>>& rows);

    Listing listing() const;
    // A private copy of the owned crop (or its mask) of a pinned or strip cell,
    // pins first, taken under the store's lock: no share of a store copy ever
    // leaves the store, so its last reference is always dropped by the thread
    // that removed it under that lock. ≤ crop_max_side² bytes. False when not
    // held or absent.
    bool image(const MonitoringCellKey& key, bool mask, cv::Mat& out, MonitoringCellInfo* info = nullptr) const;

    Stats stats() const;
    diagnostics::MemoryOwnerStats memoryStats() const;
    const Policy& policy() const { return policy_; }

    // Pure crop policy (exposed for tests): the object's bounding box grown by
    // max(cropMinMargin, the box's longer side / 2) on each side, clamped to
    // the image and to cropMaxSide around the box centre; an unknown box uses
    // the centroid, else the image centre. Empty rect for an empty image.
    static cv::Rect cropRect(int imageWidth, int imageHeight, const MonitoringCellMetrics& m, const Policy& p);

private:
    struct Entry {
        MonitoringCellInfo info;
        cv::Mat image; // owned, continuous CV_8UC1; immutable once admitted
        cv::Mat mask;
    };
    // Owned copy of a candidate (outside every lock).
    Entry copyCandidate(const MonitoringCellCandidate& c, const std::string& noImageReason) const;
    const Entry* findLocked(const MonitoringCellKey& key, bool pinsFirst) const;
    void indexLocked(const MonitoringCellKey& key, const MonitoringCellMetrics& m);
    void trimStripLocked();
    void raisePeakLocked();

    MonitoringCellSource& source_;
    const Policy policy_;

    mutable std::mutex mutex_;
    std::uint64_t scope_{0};
    std::uint64_t epoch_{0}; // bumps at beginRun / clearStrip: a refresh started before is dropped
    std::uint64_t ringGeneration_{0};
    std::uint64_t ringCaptureEpoch_{0};
    std::uint64_t revision_{0};
    std::deque<Entry> strip_; // oldest -> newest
    std::vector<Entry> pins_;
    std::uint64_t stripBytes_{0};
    std::uint64_t pinBytes_{0};
    std::uint64_t peakBytes_{0};
    std::deque<MonitoringCellKey> indexOrder_;
    std::unordered_map<MonitoringCellKey, MonitoringCellMetrics, MonitoringCellKeyHash> index_;
    Stats counters_;
};

} // namespace backend::services
