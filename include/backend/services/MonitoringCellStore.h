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
//     outside a run otherwise (a row of another run is dropped at the next
//     refresh), and only of the current capture (see below).
//   - pins: rows the operator asked to keep, by identity (start generation,
//     frame index, object id, capture epoch). A pin is refused for a run other
//     than the latest one started (and run 0, "outside a run") and for any
//     capture but the current one; all pins are dropped when the next run
//     starts (beginRun). The count and the bytes are capped and a full store
//     refuses (LimitReached): a pin is never evicted to make room for another.
//
// Captures (#716 A6a review rounds 1-3). The store is the one authority for
// the current capture epoch: advanceCapture is the one call every capture
// change goes through (a realtime start or stop, another processing ROI, a
// FrameStore replacement or camera mode switch, a camera session opening or
// closing, a frame buffer resize; beginRun is a run start's). Under the
// store's mutex it moves the epoch on, publishes it to the source (atomic
// stores: ProcessingService tags every frame with the epoch it read when the
// frame entered processing), clears the strip and the row index, and ends
// every pin: a pin keeps its identity and measurements, its pixels are
// released and it says "no image: capture E ended (reason)". Pins belong to
// the capture that made them. Every insert (strip admission, pin, row index)
// compares the row's carried epoch with the current one under the same mutex
// and drops a mismatch; every read (an image or mask, the listing) checks the
// identity's epoch against the current one under that mutex and copies in the
// same critical section. An identity of another capture is refused with
// CaptureMismatch and a plain reason, never resolved; unpin still removes an
// ended pin.
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
// its collections and its capture epoch; it is never held while the ring is
// read (only the source's lock-free publishCapture runs under it). Owned
// copies are immutable once admitted and never shared, neither outside the
// store (image() hands out a private copy taken under the store's mutex) nor
// between the strip and the pins (a pin of a strip cell copies the strip's
// copy under the mutex, #789): every buffer has one owner, which frees it
// after removing it under the mutex. These rules
// order every free of a pixel buffer after its readers by a lock, the only
// synchronisation ThreadSanitizer can see (cv::Mat's reference count lives in
// the uninstrumented OpenCV library). Lock order: a caller's (the coordinator,
// ProcessingService's rtMutex_ and capture authority mutex) -> the store's.

#include "backend/diagnostics/MemoryBudget.h"
#include "backend/processing/ProcessingTypes.h"

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backend::services {

class ProcessingService;

// Identity of one monitoring row (one detected object), stable across
// snapshots: the start generation of the run active when the row's frame
// entered processing (0 = outside a run), its frame index, its object id, and
// the capture epoch the frame entered with (frame indices restart when the
// capture changes, the epoch never repeats; 0 = a frame that crossed a
// capture change, never a cell).
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
        std::uint64_t ringGeneration{0}; // the run tag frames entering now get (the active run, or 0)
        std::vector<MonitoringCellCandidate> rows; // each with the run and capture its frame carried
    };
    virtual ~MonitoringCellSource() = default;
    // The published capture epoch (the store starts from it); lock-free.
    virtual std::uint64_t currentCaptureEpoch() const = 0;
    // The store's new epoch (and camera session floor) for the frames entering from now on.
    // Called under the store's mutex: atomic stores only, no lock, no call out.
    virtual void publishCapture(std::uint64_t captureEpoch, std::optional<std::uint64_t> cameraSessionFloor) = 0;
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

// The production source: ProcessingService::visitMonitoringNewest over the monitoring ring, and
// ProcessingService::publishMonitoringCapture for the epoch.
std::unique_ptr<MonitoringCellSource> makeProcessingCellSource(ProcessingService& processing);

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
    std::string noImageReason; // plain reason, set when !hasImage (e.g. a pin whose capture ended)
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
        CaptureMismatch = 6,    // the identity names another capture than the current one: refused, nothing changed
    };
    struct PinResult {
        PinOutcome outcome{PinOutcome::NotFound};
        std::string message;
        std::optional<MonitoringCellInfo> cell; // the pin (Pinned, PinnedWithoutImage, AlreadyPinned)
    };

    // A read refused or served (image, unpin): the reason in plain words when not ok.
    struct ReadResult {
        bool ok{false};
        bool captureMismatch{false}; // the identity names another capture than the current one
        std::string message;
        explicit operator bool() const { return ok; }
    };

    // The last capture change (advanceCapture or beginRun).
    struct CaptureChange {
        std::uint64_t captureEpoch{0}; // the capture that began
        std::string reason;            // why, in plain words
        std::size_t pinsEnded{0};      // pins kept with their measurements only
        std::size_t pinsDropped{0};    // pins removed (a run start)
        std::size_t stripDropped{0};
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
        std::uint64_t ringCaptureEpoch{0}; // the current capture: every strip cell is of it
        std::uint64_t revision{0};        // bumps whenever the strip or the pins change
        std::vector<MonitoringCellInfo> strip; // oldest -> newest
        std::vector<MonitoringCellInfo> pins;  // in pin order; a pin of an ended capture has no image
        std::optional<CaptureChange> lastCaptureChange;
        Stats stats;
        Policy policy;
    };

    explicit MonitoringCellStore(MonitoringCellSource& source);
    MonitoringCellStore(MonitoringCellSource& source, Policy policy);
    MonitoringCellStore(const MonitoringCellStore&) = delete;
    MonitoringCellStore& operator=(const MonitoringCellStore&) = delete;

    // A run started (ExperimentCoordinator, at Active): a capture change (as
    // advanceCapture, reason "a run started") that drops the pins instead of
    // ending them, and pins now belong to this run.
    void beginRun(std::uint64_t startGeneration);
    std::uint64_t scopeGeneration() const;

    // The capture changed: the one call every capture change goes through
    // (see the top of this file). Under the store's mutex, atomically: the
    // epoch moves on and is published to the source, the strip and the row
    // index are cleared, every pin is ended (measurements only; reason
    // "no image: capture E ended (reason)"). The copies are freed after the
    // mutex is released. `cameraSessionFloor`: the lowest camera session
    // whose frames still belong to a capture (a session opening or closing).
    // Returns the new epoch. Safe from any thread; never on the hot path.
    std::uint64_t advanceCapture(const std::string& reason,
                                 std::optional<std::uint64_t> cameraSessionFloor = std::nullopt);
    std::uint64_t currentCapture() const;

    // Bring the strip up to date with the ring (bounded: at most stripMaxCells
    // new crops). Called on read by the bridge, never from the hot path.
    void refreshStrip();
    // monitoring_clear: the ring was cleared, so is the strip (and the index).
    void clearStrip();

    // `key.startGeneration` is the run the caller expects the row to belong to
    // (0 = outside a run) and `key.captureEpoch` the capture it was detected in.
    PinResult pin(const MonitoringCellKey& key);
    // ok: the pin was removed (a pin of an ended capture too). Not ok: it is not held;
    // captureMismatch when the identity names another capture (its pin was dropped at a run start).
    ReadResult unpin(const MonitoringCellKey& key);
    std::size_t clearPins();

    // Rows the monitoring snapshot served (metadata only): what a pin of an
    // evicted row can still report. Bounded FIFO of kRowIndexMax rows.
    void noteRows(const std::vector<std::pair<MonitoringCellKey, MonitoringCellMetrics>>& rows);

    // One critical section: the current capture, then the cells. The strip
    // holds only the current capture's cells; pins of an ended capture are
    // listed without an image.
    Listing listing() const;
    // A private copy of the owned crop (or its mask) of a pinned or strip cell,
    // pins first: the capture check, the lookup and the copy happen in one
    // critical section of the store's mutex, so a capture change either
    // precedes the read (refused) or follows it. No share of a store copy ever
    // leaves the store, so its last reference is always dropped by the thread
    // that removed it under that lock. ≤ crop_max_side² bytes. Refused
    // (captureMismatch) when the identity names another capture than the
    // current one, whatever the store still holds; not ok with a reason when
    // the cell or that layer is not held.
    ReadResult image(const MonitoringCellKey& key, bool mask, cv::Mat& out, MonitoringCellInfo* info = nullptr) const;

    // Test seam (review round 3): called under the store's mutex where a read
    // has resolved what it serves and is about to copy it ("image", "listing");
    // and (#789) by refreshStrip after the mutex is released while the strip
    // cells it dropped are still held, just before they are freed
    // ("refresh-dropped", only when it dropped any).
    void setReadHookForTests(std::function<void(const char* where)> hook);

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
    // The capture change itself (advanceCapture, beginRun): the next epoch, published; the strip
    // and the index cleared; the pins ended (or, `dropPins`, removed). The copies are moved into
    // `freed` (released by the caller once the lock is released). Returns the new epoch.
    std::uint64_t advanceLocked(const std::string& reason, std::optional<std::uint64_t> cameraSessionFloor,
                                bool dropPins, std::vector<cv::Mat>& freed);
    // "<what>: frame F object O is from capture E, but the current capture is C (why)..."
    std::string captureRefusalLocked(const char* what, const MonitoringCellKey& key) const;
    void trimStripLocked();
    void raisePeakLocked();

    MonitoringCellSource& source_;
    const Policy policy_;

    mutable std::mutex mutex_;
    std::uint64_t scope_{0};
    std::uint64_t epoch_{0}; // bumps at clearStrip / a capture change: a refresh started before is dropped
    std::uint64_t ringGeneration_{0};
    std::uint64_t current_{0}; // the current capture epoch, owned here (the source's at construction)
    std::optional<CaptureChange> lastChange_;
    std::function<void(const char*)> readHook_; // tests only
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
