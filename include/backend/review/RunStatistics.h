// Run statistics: the per-run descriptive numbers of a closed HDF5 experiment
// run (#716 A7a, the issue's A7 rulings; bridge ABI 44 `run_quick_look`,
// review ABI 6 `review_run_statistics`). One function computes them for every
// surface: the operation app's Run summary and Quick look (O5/O6, through the
// coordinator's pass at close and `run_quick_look`) and Review's twin, so their
// parity holds by construction. Qt-free; lives in `mib_review_core`.
//
// What it computes, over the valid set (`/valid_frames/metadata`, the rows
// with isValid, as Review's charts count them):
//  - counts: the set's rows, the valid rows, the finite rows (area and
//    deformability both finite: the points Review plots) and the excluded
//    rows (valid minus finite); the run accounting as the file records it;
//  - area in µm²: n and median, at the file's RECORDED factor only
//    (ReviewSession::recordedPixelToMicron); with none it is not recorded,
//    never a fallback;
//  - deformability: n and median;
//  - E in kPa: status computed | no_lut | not_recorded (from the run snapshot
//    v3 `emodulus_lut` or a regenerated file's `@emodulus_lut_json`, #764),
//    n and median over finite values > 0, out_of_domain (valid rows whose E is
//    not finite: EModulusLut::lookup's NaN outside its table), the LUT identity;
//  - the target group: declared (the recorded config), n of N valid rows, %;
//  - the event rate: valid rows per second over their timestamp span on the
//    file's declared clock (acquisition provenance ticks per second);
//  - the identity (ReviewSession::readRunIdentity) and descriptive flags;
//  - an overlay of at most kRunStatisticsOverlayMaxPoints finite rows at a
//    deterministic stride: display only, outside the parity claim.
// Nothing is fabricated: absent is null ("not recorded"), a recorded 0 stays
// 0. Nothing is written anywhere: not the file, not a store, not a snapshot.
// No tests, fits, CV/IQR or verdicts (out of scope, ruling 10).
//
// Threading: the pure parts (median, the accumulator, computeRunStatistics,
// the deltas, the JSON) are reentrant. The readers use HDF5, which is built
// without thread safety (#722): callers serialize them with every other HDF5
// user (the coordinator's finalization, ReviewSession's mutex under the review
// lanes' lock, ReviewJobs::runStatistics). RunStatisticsCache is thread-safe.
#pragma once

#include "backend/review/ReviewTypes.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backend::services
{
    class Hdf5Service;
    struct ProcessedFrame;
}

namespace backend::review
{

    // Contract-pinned (bridge-contract.json run_quick_look, review-contract.json
    // review_run_statistics; static_asserts in shim.cpp / review_shim.cpp).
    inline constexpr std::size_t kRunQuickLookMaxPaths = 32;
    inline constexpr std::size_t kRunQuickLookPathMaxBytes = 4096;
    inline constexpr std::size_t kRunStatisticsOverlayMaxPoints = 2000;
    inline constexpr std::uint64_t kRunStatisticsLowN = 200; // `low_n` below this many valid rows (Q6 a)
    inline constexpr std::size_t kRunStatisticsCacheEntries = 64;
    inline constexpr std::uint32_t kRunStatisticsSchemaVersion = 1;

    // The median of `values` (the middle value; the mean of the two middle values
    // for an even count). Precondition: not empty. Shared with
    // app::deriveCalibration (CalibrationRecord.cpp), so a calibration record's
    // medians and the quick look's are the same arithmetic.
    double median(std::vector<double> values);

    // The rows of a valid set, accumulated in set order: the one place both read
    // paths feed (the column reader, readRunStatisticsRows, and ReviewSession's
    // metadata rows, runStatisticsRowsFromFrames), so a difference between them
    // can only be a difference in the values read. Keeps per valid row its area
    // and deformability (the medians and the overlay need them), its target-group
    // bit (the overlay) and only finite positive E values; everything else is a
    // counter: about 17 bytes per valid row plus 8 per E value.
    class RunStatisticsRows
    {
    public:
        void add(bool isValid, double areaPx, double deformability, double youngsModulusKpa, bool targetGroup,
                 std::uint64_t timestamp);
        std::uint64_t setRows() const { return setRows_; }
        std::uint64_t validRows() const { return validRows_; }

    private:
        friend struct RunStatisticsRowsAccess;
        friend struct RunStatistics computeRunStatistics(RunStatisticsRows rows, const struct RunStatisticsHeader &header);
        std::uint64_t setRows_{0};
        std::uint64_t validRows_{0};
        std::uint64_t finiteRows_{0};
        std::uint64_t targetGroupRows_{0};
        std::uint64_t eNonFinite_{0};
        bool haveTimestamp_{false};
        std::uint64_t timestampMin_{0};
        std::uint64_t timestampMax_{0};
        std::vector<double> areaPx_;        // per valid row, set order
        std::vector<double> deformability_; // per valid row, parallel to areaPx_
        std::vector<std::uint8_t> targetGroup_; // per valid row, parallel (the overlay's colour)
        std::vector<double> eKpa_;          // finite values > 0 of valid rows
    };

    // What a file records about itself, attributes only (no metadata rows).
    struct RunStatisticsHeader
    {
        bool recordingFile{false}; // a raw recording: no objects, "not available until exported to HDF5"
        RunIdentity identity;
        // The factor the objects were recorded at; 0 = not recorded.
        double recordedPixelToMicron{0.0};
        // The E-LUT record of the stored E (#764); nullopt = not recorded.
        std::optional<recording::ProvenancedEModulusLut> eModulusLut;
        // The metadata has a youngsModulus member (true for an absent valid set:
        // no stored row lacks it, so the E-LUT record alone decides).
        bool eFieldRecorded{false};
        bool metadataPresent{false};
        bool hasAccounting{false};
        recording::RecordingAccountingSnapshot accounting;
        std::uint64_t ticksPerSecond{0}; // the declared timestamp clock; 0 = not recorded
        // The recorded processing config (nullopt: the file records none).
        std::optional<services::ProcessingConfig> config;
        std::optional<std::string> coreVersion;
        std::optional<int> contractVersion;
        std::optional<std::string> methodRevisionId;     // snapshot method.revision_id (a central method)
        std::optional<std::string> configCanonicalSha256; // snapshot method.config_canonical_sha256
        std::optional<std::uint64_t> startGeneration;    // snapshot start_generation
        std::optional<std::uint64_t> startTimeNs;        // a recorded 0 time is not recorded
        std::optional<std::uint64_t> endTimeNs;
    };

    // The E status names (`emodulus.status`).
    inline constexpr const char *kRunEComputed = "computed";
    inline constexpr const char *kRunENoLut = "no_lut";
    inline constexpr const char *kRunENotRecorded = "not_recorded";

    struct RunStatistics
    {
        std::uint32_t schemaVersion{kRunStatisticsSchemaVersion};
        RunStatisticsHeader header;
        std::uint64_t setRows{0};
        std::uint64_t validRows{0};
        std::uint64_t finiteRows{0};
        std::uint64_t excludedRows{0};
        // Area (µm², the recorded factor only): n 0 and no median when not recorded.
        std::uint64_t areaN{0};
        std::optional<double> areaMedianUm2;
        std::uint64_t deformabilityN{0};
        std::optional<double> deformabilityMedian;
        std::string eStatus{kRunENotRecorded};
        std::uint64_t eN{0};
        std::optional<double> eMedianKpa;
        std::uint64_t eOutOfDomain{0};
        // Target group: declared nullopt = the file records no config.
        std::optional<bool> targetGroupDeclared;
        std::uint64_t targetGroupN{0};
        std::optional<double> targetGroupPercent;
        // Event rate: computed | not_recorded (no declared clock) | too_short.
        std::string eventRateStatus{"not_recorded"};
        std::optional<double> eventRatePerS;
        std::optional<double> eventRateSpanS;
        std::vector<std::string> flags;
        // Overlay (display only): every `overlayStride`-th finite row.
        std::uint64_t overlayStride{0};
        std::vector<double> overlayAreaPx;
        std::vector<double> overlayAreaUm2; // empty when the factor is not recorded
        std::vector<double> overlayDeformability;
        std::vector<std::uint8_t> overlayTargetGroup;
    };

    // The numbers of one run. Pure; consumes `rows`.
    RunStatistics computeRunStatistics(RunStatisticsRows rows, const RunStatisticsHeader &header);

    // The header of `reader`'s open file. Does not throw for a bad attribute.
    RunStatisticsHeader readRunStatisticsHeader(const services::Hdf5Service &reader);
    // The valid set of `reader`'s open file through the column reader
    // (Hdf5Service::readMetadataColumns, bounded chunks). An absent valid set
    // is empty; false (with `error`) when it exists but cannot be read.
    // `chunkRows` 0 = Hdf5Service::kMetadataColumnChunkRows (tests pass small ones).
    bool readRunStatisticsRows(const services::Hdf5Service &reader, RunStatisticsRows &out, std::string *error,
                               std::size_t chunkRows = 0);
    // The same rows from metadata-only ProcessedFrames (ReviewSession's row path).
    RunStatisticsRows runStatisticsRowsFromFrames(const std::vector<services::ProcessedFrame> &frames);

    // A file as the cache keys it: canonical path, size and modification time
    // (a stat, no HDF5 read, so it works during a run).
    struct RunFileStamp
    {
        std::string canonicalPath;
        std::uint64_t sizeBytes{0};
        std::int64_t mtimeNs{0}; // std::filesystem::file_time_type ticks (no portable epoch): a key, never reported
        bool operator==(const RunFileStamp &o) const
        {
            return canonicalPath == o.canonicalPath && sizeBytes == o.sizeBytes && mtimeNs == o.mtimeNs;
        }
    };
    // False with a plain `error` when `path` is empty, missing or not a regular file.
    bool stampRunFile(const std::string &path, RunFileStamp &out, std::string &error);

    // One file's result.
    enum class RunStatisticsStatus : std::uint32_t
    {
        Computed = 0,
        Deferred = 1,     // computed when the instrument is idle
        NotAvailable = 2, // a raw recording: not available until exported to HDF5
        Error = 3,        // the file cannot be read (message)
    };
    const char *toString(RunStatisticsStatus status);
    inline constexpr const char *kRunStatisticsDeferredMessage = "computed when the instrument is idle";
    inline constexpr const char *kRunStatisticsRawMessage = "not available until exported to HDF5";

    struct RunStatisticsResult
    {
        std::string path; // as requested
        RunStatisticsStatus status{RunStatisticsStatus::Error};
        std::string message;
        std::optional<RunFileStamp> stamp;
        std::shared_ptr<const RunStatistics> stats; // Computed only
        // Where the numbers came from: "close" (the coordinator's pass) or
        // "on_demand"; "" when not computed.
        std::string computedAt;
        bool fromCache{false};
    };
    RunStatisticsResult deferredRunStatistics(const std::string &path, std::string message = kRunStatisticsDeferredMessage);

    // One file with a short-lived reader of its own (opened, read, closed):
    // Computed, NotAvailable (a raw recording) or Error. The caller serializes
    // it with every other HDF5 user (#722); it never throws.
    RunStatisticsResult readRunStatistics(const std::string &path, const std::string &computedAt);

    // The in-memory LRU of computed runs (ruling 3): at most `capacity` entries,
    // keyed by the file stamp; each entry carries the run_id it was computed
    // with, which `find` confirms when one is expected. Not persisted. Thread-safe.
    class RunStatisticsCache
    {
    public:
        struct Entry
        {
            std::shared_ptr<const RunStatistics> stats;
            std::string computedAt;
        };
        explicit RunStatisticsCache(std::size_t capacity = kRunStatisticsCacheEntries);
        std::optional<Entry> find(const RunFileStamp &stamp);
        void insert(const RunFileStamp &stamp, Entry entry);
        std::size_t size() const;
        std::size_t capacity() const { return capacity_; }
        void clear();

    private:
        struct Node
        {
            RunFileStamp stamp;
            Entry entry;
        };
        const std::size_t capacity_;
        mutable std::mutex mutex_;
        std::list<Node> lru_; // most recent first
        std::unordered_map<std::string, std::list<Node>::iterator> byPath_;
    };

    // `run_quick_look` / `review_run_statistics` request: {paths: [1..32
    // strings], reference?: null | {path, run_id: string | null,
    // start_generation?: decimal string | null}}. Strict: unknown or duplicate
    // keys, wrong types, empty or over-long paths are refused with the key named.
    struct RunQuickLookReference
    {
        std::string path;
        std::optional<std::string> runId;
        std::optional<std::string> startGeneration; // client-held, echoed
    };
    struct RunQuickLookRequest
    {
        std::vector<std::string> paths;
        std::optional<RunQuickLookReference> reference;
    };
    bool parseRunQuickLookRequest(const std::string &json, RunQuickLookRequest &out, std::string &error);

    // The reply document both commands return (bridge-contract.json
    // run_quick_look.reply): one entry per path in order and the reference,
    // with `run − reference` deltas per median where the rulings allow them.
    // `reference` (when the request named one) is confirmed against its run_id
    // here. `ok` false only for a refused request (`message`).
    std::string runQuickLookReplyJson(const std::vector<RunStatisticsResult> &runs,
                                      const std::optional<RunQuickLookReference> &referenceRequest,
                                      const std::optional<RunStatisticsResult> &reference);
    std::string runQuickLookRefusedJson(const std::string &message);
    // The `statistics` member of one run as the reply carries it (compact JSON;
    // the parity tests compare it byte for byte).
    std::string runStatisticsJson(const RunStatistics &stats);

    // Test-only access to the accumulator.
    struct RunStatisticsRowsAccess
    {
        static const std::vector<double> &areaPx(const RunStatisticsRows &rows) { return rows.areaPx_; }
    };

} // namespace backend::review
