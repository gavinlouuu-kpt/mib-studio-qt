#pragma once

// The SATA SSD record store as Studio sees it (#667 S1, read only): the state word (R1), the counters of an open run (R3) and the run table
// (R4, R5), all through `libpzrec` / the `pzrec` CLI of the board owner (pz7035 tools/pzrec, JSON is the contract). Studio never sequences
// the drain or touches registers here: `ISsdDevice` is the one seam, with the CLI on an image or a disk as the implementation (the fake
// disk of the tests and, later, the board's block device and PL window behind the same CLI/library).
//
// Honesty rules: a pzrec failure (non-zero exit, a timeout, JSON that does not parse or lacks the contract's fields) is WEDGED with the stderr
// as the reason, never READY. No device configured is ABSENT ("No SSD: nothing can be saved"). A run whose totals are unknown (mount-time recovery,
// reason 7, `counts_unknown`) carries no numbers for the UI.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backend::pz {

enum class SsdState {
    Absent, Initialising, Recovering, Ready, Recording, Stopping, Wedged, RunTableFull, RawFull, Unformatted,
};
const char* ssdStateName(SsdState s);
// The state names of pzrec's JSON ("READY", "RAW_FULL", ...); nullopt-like: returns false for an unknown name.
bool parseSsdState(const std::string& name, SsdState& out);

struct SsdCounters {
    uint64_t seen{0}, emptyFiltered{0}, invalidNotSampled{0}, passed{0}, written{0}, dropped{0}, failed{0}, bytesWritten{0};
    uint64_t drainKbps{0}, firstFrameId{0}, lastFrameId{0};
};

struct SsdRun {
    uint32_t id{0};
    bool open{false}, deleted{false}, incomplete{false};
    uint64_t startLba{0}, endLba{0};
    uint64_t startUnixMs{0};
    uint32_t wallSource{0}, clientTag{0}, filter{0}, samplerN{0};
    uint64_t recSectors{0};      // the size of one record in sectors (116 for 59,392 bytes), NOT the run's size: that is endLba - startLba
    uint64_t firstFrameId{0}, lastFrameId{0}, firstTicks{0}, lastTicks{0};
    uint32_t tickHz{0};
    uint64_t seen{0}, emptyFiltered{0}, invalidNotSampled{0}, passed{0}, written{0}, dropped{0}, failed{0};
    uint32_t recoveries{0};
    uint32_t reason{0};          // completion reason (0 clean, 1 drain fault, 5 not stopped, 6 length limit, 7 recovered at mount, 8 abort, 9 space limit)
    bool countsUnknown{false};   // reason 7: the totals are not known, no numbers are shown
};

struct SsdStatus {
    SsdState state{SsdState::Absent};
    std::string lastError;       // pzrec's last_error name ("OK", ...)
    std::string reason;          // the UI text when the state is not usable (why absent, wedged, ...), empty when Ready/Recording
    uint64_t rawSectors{0}, headLba{0}, tailLba{0}, freeSectors{0}, minRunSectors{0};
    uint32_t runs{0}, nextRunId{0}, tableEntries{0};
    bool openRun{false};
    uint32_t openRunId{0};
    uint64_t openStartUnixMs{0};
    uint32_t openClientTag{0}, openWallSource{0};
    uint32_t recoveredRuns{0}, skippedBadEntries{0};
    std::vector<uint32_t> recoveredIds;
    SsdCounters counters;
    bool ok() const { return state == SsdState::Ready || state == SsdState::Recording || state == SsdState::Stopping; }
};

// Parsers of the CLI's JSON (pure; tests feed them hostile text). On failure they return false and say why.
bool parseSsdStatus(const std::string& json, SsdStatus& out, std::string* why);
bool parseSsdRuns(const std::string& json, std::vector<SsdRun>& out, std::string* why);

class ISsdDevice {
public:
    virtual ~ISsdDevice() = default;
    // One pzrec call each; false with `error` (the stderr or the reason it could not run) on any failure.
    virtual bool status(std::string& json, std::string* error) = 0;
    virtual bool runs(std::string& json, std::string* error) = 0;
};

// `pzrec <verb> <image>` run as a child process with a bounded time. POSIX only; elsewhere every call fails ("pzrec is not available on this platform").
class PzrecCliDevice final : public ISsdDevice {
public:
    PzrecCliDevice(std::string pzrecPath, std::string imagePath, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    bool status(std::string& json, std::string* error) override;
    bool runs(std::string& json, std::string* error) override;
private:
    bool call(const char* verb, std::string& json, std::string* error);
    std::string pzrec_, image_;
    std::chrono::milliseconds timeout_;
};

// Runs a command with a timeout, capturing stdout and stderr (exposed for the tests of the timeout and exit-code handling).
struct RunResult { bool started{false}; bool timedOut{false}; int exitCode{-1}; std::string out, err; };
RunResult runBounded(const std::vector<std::string>& argv, std::chrono::milliseconds timeout);

// The store never blocks its callers on the CLI: with `background` a thread refreshes the status every `interval` (and the run table when the status says it
// changed, or every 10 intervals), and status()/runs() return the last result at once (a hung pzrec costs the thread, not the bridge lock). Without
// `background` (tests) status() refreshes inline when the cache is older than `interval`. Before the first refresh the state is INITIALISING.
class SsdStore {
public:
    // `device` null: no SSD configured (ABSENT).
    // `stateFile` (on the eMMC, may be empty): where the highest run id Studio has seen is kept, so that "recovered at mount" is told once after a restart.
    explicit SsdStore(std::unique_ptr<ISsdDevice> device, std::chrono::milliseconds interval = std::chrono::milliseconds(1000), bool background = false,
                      std::string stateFile = {});
    ~SsdStore();
    SsdStore(const SsdStore&) = delete;
    SsdStore& operator=(const SsdStore&) = delete;
    SsdStatus status();
    // The run table, oldest first, as of the last refresh. False with `why` (and an empty list) when it is not readable.
    bool runs(std::vector<SsdRun>& out, std::string* why);
    // Runs pzrec now (status, then the run table when needed) and updates the cache; the thread calls it, tests call it directly.
    void refresh(bool force = false);
    bool configured() const { return device_ != nullptr; }
private:
    SsdStatus statusLocked() const;
    std::unique_ptr<ISsdDevice> device_;
    std::chrono::milliseconds interval_;
    bool background_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stop_{false};
    bool haveStatus_{false};
    SsdStatus status_;
    std::vector<SsdRun> runs_;
    std::string runsWhy_{"SSD run table not read yet"};
    bool haveRuns_{false};
    uint64_t runsSignature_{0};
    int ticksSinceRuns_{0};
    // "Recovered at mount" comes from the run table, never from pzrec's one-shot status fields: a run with reason 7 is the recovery, for as long as the
    // entry exists. The notice after a restart names the reason-7 runs whose id is above the highest id Studio saw before (`seenBaseline_`, read once
    // from `stateFile_`); the highest id seen now is written back.
    std::string stateFile_;
    bool baselineLoaded_{false};
    uint32_t seenBaseline_{0}, seenPersisted_{0};
    std::vector<uint32_t> noticeIds_;
    std::chrono::steady_clock::time_point statusAt_{};
};

} // namespace backend::pz
