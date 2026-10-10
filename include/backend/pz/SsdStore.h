#pragma once

// The SATA SSD record store as Studio sees it (#667 S1, read only): the state word (R1), the counters of an open run (R3) and the run table
// (R4, R5), all through `libpzrec` / the `pzrec` CLI of the board owner (pz7035 tools/pzrec, JSON is the contract). Studio never sequences
// the drain or touches registers here: `ISsdDevice` is the one seam, with the CLI on an image or a disk as the implementation (the fake
// disk of the tests and, later, the board's block device and PL window behind the same CLI/library).
//
// Honesty rules: a pzrec failure (non-zero exit, a timeout, JSON that does not parse or lacks the contract's fields) is WEDGED with the stderr
// as the reason, never READY. No device configured is ABSENT ("No SSD: nothing can be saved"). A run whose totals are unknown (mount-time recovery,
// reason 7, `counts_unknown`) carries no numbers for the UI.

#include <atomic>
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
    bool totalsInconsistent{false}; // the totals do not add up (seen = empty + invalid not sampled + passed; passed = written + dropped + failed): no numbers either
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

// The arguments of `pzrec start` (R2). The PC rule is VALID_ONLY with the invalid sampler; `startUnixMs` comes from the WallClock (G14) and `clientSynced`
// says whether a client set it (pzrec's `wall_source` 1) or the board's unsynchronised clock did (0).
enum class SsdFilter { All, AnyResult, ValidOnly };
struct SsdStartArgs {
    SsdFilter filter{SsdFilter::ValidOnly};
    uint32_t samplerN{0};
    uint32_t clientTag{0};
    uint64_t startUnixMs{0};
    bool clientSynced{false};
};

class ISsdDevice {
public:
    virtual ~ISsdDevice() = default;
    // One pzrec call each; false with `error` (the stderr or the reason it could not run) on any failure.
    virtual bool status(std::string& json, std::string* error) = 0;
    virtual bool runs(std::string& json, std::string* error) = 0;
    // `pzrec start` / `pzrec stop [--abort]`: on success `json` is the status object the CLI prints after the call. A fake that does not record fails them.
    virtual bool start(const SsdStartArgs&, std::string& json, std::string* error) { (void)json; if (error) *error = "this SSD device cannot start a run"; return false; }
    virtual bool stop(bool abort, std::string& json, std::string* error) { (void)abort; (void)json; if (error) *error = "this SSD device cannot stop a run"; return false; }
    // The live window state from the registers alone (`pzrec snapshot --window-only`): never opens the disk. Fails ("no reading") when the window is absent.
    virtual bool windowSnapshot(std::string& json, std::string* error) { (void)json; if (error) *error = "this SSD device has no window to read"; return false; }
    // The command line of `pzrec read RUN IMG ARGS --summary [--from F] [--count N]` (RUN before IMG): one argument per element, never a shell string.
    virtual bool readArgv(uint32_t run, uint64_t from, uint64_t count, std::vector<std::string>& argv) { (void)run; (void)from; (void)count; (void)argv; return false; }
};

// `pzrec <verb> <image>` run as a child process with a bounded time. POSIX only; elsewhere every call fails ("pzrec is not available on this platform").
class PzrecCliDevice final : public ISsdDevice {
public:
    // `extraArgs` follow the image on every call (the board's `--hw pl --bounce-phys ...`; the CLI reads its options anywhere after IMG).
    // `startTimeout` / `stopTimeout` bound the two calls that sequence the drain; a stop that outlives its timeout is killed and retried by the store.
    PzrecCliDevice(std::string pzrecPath, std::string imagePath, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000),
                   std::vector<std::string> extraArgs = {}, std::chrono::milliseconds startTimeout = std::chrono::milliseconds(15000),
                   std::chrono::milliseconds stopTimeout = std::chrono::milliseconds(30000));
    bool status(std::string& json, std::string* error) override;
    bool runs(std::string& json, std::string* error) override;
    bool start(const SsdStartArgs& args, std::string& json, std::string* error) override;
    bool stop(bool abort, std::string& json, std::string* error) override;
    bool windowSnapshot(std::string& json, std::string* error) override;
    bool readArgv(uint32_t run, uint64_t from, uint64_t count, std::vector<std::string>& argv) override;
private:
    bool call(const std::vector<std::string>& verbAndArgs, std::chrono::milliseconds timeout, std::string& json, std::string* error);
    std::string pzrec_, image_;
    std::chrono::milliseconds timeout_, startTimeout_, stopTimeout_;
    std::vector<std::string> extra_;
};

// Runs a command with a timeout, capturing stdout and stderr (exposed for the tests of the timeout and exit-code handling).
struct RunResult { bool started{false}; bool timedOut{false}; bool truncated{false}; int exitCode{-1}; std::string out, err; };  // truncated: more than 4 MiB on a pipe
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
    // The filter of the runs Studio starts (MIB_SSD_FILTER = all|any|valid; default valid, the PC rule). A value that is none of them is a configuration problem:
    // `configProblem()` is non-empty and prepareRun() refuses, so a typo never records with another filter than the one asked for.
    SsdFilter filter() const { return filter_; }
    const std::string& configProblem() const { return configProblem_; }
    // Parses the setting; nullptr/empty = valid. False (with `problem`) for anything else.
    static bool parseFilter(const char* text, SsdFilter& out, std::string* problem);
    void setFilterFromEnv(const char* text);

    // ---- Record (#667 S2) ----------------------------------------------------------------------------------------------------------------------------
    // The start sequence of an SSD run, in the caller's order: prepareRun() (a fresh `pzrec status` under a short timeout; READY and no open run, else a
    // refusal that names the state, never a retry loop) returns the run-table id N that pzrec_start will allocate; the caller writes N to the bridge's RUN_ID
    // before ARM; startRun(N, args) runs `pzrec start` after the ARM and checks that the run it opened has id N.
    struct Prepared { bool ok{false}; uint32_t runId{0}; std::string why; SsdState state{SsdState::Absent}; };
    Prepared prepareRun();
    // `opened` is true when pzrec opened a run even though startRun fails (a different id): the caller must stop it.
    bool startRun(uint32_t expectedId, const SsdStartArgs& args, std::string* why, bool* opened = nullptr);
    // The stop never blocks its caller: beginStop() starts a thread that runs `pzrec stop` with a timeout per attempt and retries (a stop took 19 minutes on the
    // library before pz7035 #48); waitStopped() waits for it under a deadline. Until it ends status() reads STOPPING. A stop that fails every attempt keeps the
    // run's id and the reason (stopResult()) and the next refresh shows what pzrec says; Studio does not claim the run was closed.
    void beginStop(bool abort = false);
    bool stopping() const;
    // true: the run is closed. false with `why`: still stopping (`stillRunning` true) or every attempt failed.
    bool waitStopped(std::chrono::milliseconds deadline, std::string* why, bool* stillRunning = nullptr);
    // The run this store opened and has not seen closed (0: none).
    uint32_t openedRunId() const;
    void setStopRetries(int attempts, std::chrono::milliseconds backoff) { stopAttempts_ = attempts; stopBackoff_ = backoff; }

    // ---- Export (#667): download of one closed run's raw records, idle only -------------------------------------------------------------------------
    // One lease covers the whole download: taken before the reader process is spawned (the open phase included) and released by exportEnd() after that process
    // has exited, so the periodic refresh, prepareRun and the export never touch the window and the disk (one bounce buffer) at the same time. Taking it refuses,
    // with the reason, while Studio records (own run, a Start in progress, STOPPING, a stop that failed), the window shows a run active, or another lease is held.
    // The lease expires on its own (`maxSeconds` + kExportKillMarginSeconds from the start of the download) so that a crashed caller cannot hold the SSD for ever;
    // the caller terminates its reader at `maxSeconds` (SIGTERM, SIGKILL) whether or not the client reads, so the reader is dead when the lease expires.
    struct ExportBegin {
        bool ok{false};
        uint64_t lease{0};
        uint32_t runId{0};
        uint64_t records{0}, bytes{0};       // what the body holds: `records` x 59,392 B
        uint32_t maxSeconds{0};
        std::vector<std::string> argv;
        std::string reason;                  // why not, when !ok
        std::string code;                    // BUSY (a recording, a Start, another export: try later), UNAVAILABLE (the SSD or the window cannot be read), NO_SUCH_RUN,
                                             // RUN_DELETED, NOT_OFFERED (open, recovered without a count, empty), BAD_RANGE
    };
    static constexpr uint64_t kRecordBytes = 59392;
    // `count` 0: to the end of the run. `from` and `count` must lie inside the run's `written` records (refused, never clipped).
    ExportBegin exportBegin(uint32_t run, uint64_t from = 0, uint64_t count = 0);
    // Idempotent: an unknown or already released lease is a no-op. `bytesSent` and `outcome` are for the log only.
    void exportEnd(uint64_t lease, uint64_t bytesSent, const std::string& outcome);
    bool exportActive();
    // A Start that did not reach startRun() (a failure between prepareRun and the start) calls this; after startRun() it is a no-op.
    void cancelPrepare();
    // The lease outlives the reader's own bound (`maxSeconds`, when the caller starts to terminate it) by this much: SIGTERM grace 35 s + SIGKILL wait 10 s + 15 s margin, so the
    // lease cannot expire while the reader is still alive inside a block command (two disk users).
    static constexpr uint32_t kExportKillMarginSeconds = 60;
    // Tests: the lease bound, normally derived from the run's size, and the margin after it.
    void setExportMaxSecondsForTesting(uint32_t seconds, uint32_t marginSeconds = kExportKillMarginSeconds) {
        exportMaxSecondsOverride_ = seconds;
        exportMarginSeconds_ = marginSeconds;
    }
private:
    void stopWorker(bool abort);
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
    SsdFilter filter_{SsdFilter::ValidOnly};
    std::string configProblem_;
    // Record state. Every pzrec call is serialised on deviceMutex_: two processes must never sequence the window or the disk at once.
    mutable std::mutex deviceMutex_;
    mutable std::mutex stopMutex_;
    std::condition_variable stopCv_;
    std::thread stopThread_;
    std::atomic<bool> stopRunning_{false}, shuttingDown_{false}, stopFailed_{false};
    bool stopDone_{false}, stopOk_{false};
    std::string stopWhy_;
    uint32_t openedRunId_{0};            // guarded by m_
    // Export lease (guarded by m_). `prepared_`: a Start passed prepareRun() and has not reached startRun() yet (a stale mark expires after 120 s: a Start that died between the two calls must not block downloads for ever; a live Start that is slower is still safe, because exportBegin then waits for deviceMutex_ behind its pzrec start and sees the drain ARMED/DRAINING in the window snapshot).
    uint64_t exportLease_{0}, nextLease_{1}, exportBytes_{0};
    uint32_t exportRun_{0}, exportMaxSecondsOverride_{0}, exportMarginSeconds_{kExportKillMarginSeconds};
    std::chrono::steady_clock::time_point exportDeadline_{};
    bool prepared_{false};
    std::chrono::steady_clock::time_point preparedAt_{};
    bool exportHeldLocked();             // expires an overdue lease (logs it); true while one is held
    int stopAttempts_{5};
    std::chrono::milliseconds stopBackoff_{2000};
};

} // namespace backend::pz
