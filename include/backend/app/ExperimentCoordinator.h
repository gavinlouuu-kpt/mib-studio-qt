// Backend-owned experiment readiness + Start/Stop transaction (issue #369;
// host-SDK portion of #274).
//
// One serialized transaction owner: evaluateReadiness() produces a
// generation-tagged ExperimentReadinessSnapshot from the actual backend state;
// start() re-evaluates, refuses a stale generation (any invalidation input
// changed since the presented evaluation), opens persistence, freezes the
// immutable RunConfigurationSnapshot, persists it, and only then enters
// Active. Failure at any step rolls back without publishing a run.
//
// The coordinator also owns the run's lifetime after Start (issue #372 G2/G3):
// a worker thread runs the periodic flush while Active and the whole
// finalization on requestStop() (drain, metadata, accounting, provenance,
// close), so no client (Qt window or bridge) touches Hdf5Service for a run.
#pragma once

#include "backend/app/CalibrationRecord.h"
#include "backend/app/DestinationWriteRate.h"
#include "backend/app/RecordingCapacity.h"
#include "backend/app/WallClock.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/review/RunStatistics.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace backend {
class AppBackend;
}

namespace backend::app {

struct ExperimentStartRequest {
    std::string outputPath;         // HDF5 destination (extension normalized)
    uint64_t readinessGeneration{0}; // generation the caller preflighted with
    std::string profileId;          // frontend profile identity (provenance only)
    bool acknowledgeLatestFrameDrops{false}; // operator accepted LatestFrame policy
    bool acknowledgeDiscardRing{false};      // operator accepted that a stopped Run's buffered frames are discarded by this start (#693)
    // What the client declares about the run (#712, ABI 39): experiment / run / sample identity, the
    // selected operator and the applied calibration record (into the run snapshot v3), and the
    // planned annotation (written once into /experiment_annotation). Checked by
    // recording::validateStartRequest; a refused one is InvalidRequest and nothing is opened.
    recording::StartRequestIdentity declared;
};

class ExperimentCoordinator {
public:
    explicit ExperimentCoordinator(AppBackend& backend);
    ~ExperimentCoordinator();

    // Application identity recorded in every run snapshot.
    void setApplicationIdentity(std::string version, std::string buildId, std::string os);

    // Evaluate every gate against the current backend state. Cheap; safe to
    // call from a UI timer. Each call increments the readiness generation
    // only when an invalidation input changed (so a stable state keeps its
    // generation and a preflight stays usable).
    // `outputPath` is accepted as Start accepts it (acceptOutputPath, #762): the
    // gates, the probes and the candidate see the resolved absolute path.
    ExperimentReadinessSnapshot evaluateReadiness(const std::string& outputPath = {},
                                                  const std::string& profileId = {});

    // The Start transaction. Serialized; TOCTOU-free with respect to the
    // presented readiness generation.
    ExperimentStartResult start(const ExperimentStartRequest& request);

    // Recording capacity for the operator's run setup (#833): the camera
    // window, rate and buffer plan with the camera.window, camera.rate,
    // storage.buffer and storage.throughput gates, as the next evaluation
    // would give them. Cheap, no generation change, no disk write: the
    // destination rate is the cached measurement (the destination's, or the
    // last one measured) and is measured only by evaluateReadiness/start
    // with an output path. `known` is false while no frame geometry is known
    // (camera not running) or on a PL-science instrument.
    //
    // `planned` (the Overview tab, where the camera shows the full sensor):
    // the window being placed instead of the running session's frames. The
    // session's rates are used only when it runs that window size outside an
    // Overview; otherwise the rate is the profile's target_fps and the
    // camera maximum is unknown until the Experiment script applies it.
    struct RecordingCapacityView {
        bool busy{false};    // a Start or finalization held the coordinator: nothing evaluated
        bool known{false};
        bool planned{false}; // describes `planned`, not the running session
        RecordingCapacityInputs inputs;
        RecordingCapacity capacity;
        ReadinessGate window;
    };
    RecordingCapacityView recordingCapacity(const std::string& outputPath = {},
                                            std::optional<::camera::common::CameraWindow> planned = std::nullopt);

    // The destination write-rate measurement (storage.throughput); tests
    // install a fake. nullptr restores measureDestinationWriteRate. A slow
    // result is measured again (up to three probes, the best kept, #841) and
    // the result is cached per (destination directory, saved frame size) unless
    // it fails the floor stream: that one is probed again at the next Start.
    using WriteRateProbe = std::function<WriteRateMeasurement(const std::string& outputPath, uint64_t bytesPerFrame)>;
    void setWriteRateProbe(WriteRateProbe probe);

    // Ask the worker to finalize the active run. Returns immediately;
    // completion is observed through status()/the status callback
    // (terminal == true). Busy while a finalization is already running,
    // NotActive when there is no run.
    ExperimentStopOutcome requestStop(bool cancelled);

    // The same, for one named run (#741, bridge ABI 40). With an expected start generation the
    // check and the stop are one critical section under mutex_ (the lock start() and the
    // worker's finalization hold), so a stale generation can never stop a newer run: when the
    // coordinator holds no run (Idle, Failed, Starting) or holds one with another generation
    // (Active, Stopping, or a failed run still finalizing), the result is RunMismatch, nothing
    // changes and `refusal` (when given) receives a plain reason. When the generation matches,
    // the outcome is exactly the one-argument outcome (Accepted, Busy, NotActive). Without one,
    // this is requestStop(cancelled).
    ExperimentStopOutcome requestStop(bool cancelled, std::optional<uint64_t> expectedStartGeneration,
                                      std::string* refusal = nullptr);

    // Z stage (#533): Start is refused while the probe says a stage operation is active, so
    // an experiment never starts under a moving stage. The probe runs with the coordinator
    // lock held (lock order: coordinator, then stage), so it must not call back in. Stage
    // moves are queued under the same lock (withIdleConfiguration), which makes the check
    // and the queueing atomic against each other.
    void setStageBusyProbe(std::function<bool()> probe);

    // Run quick look at close (#716 A7a, finalization step 6b): after the calibration record and
    // before the terminal status, the run's statistics (review/RunStatistics.h) are computed from
    // the closed file and put in AppBackend's in-memory cache (runStatisticsCache); nothing is
    // written to the file, a store or the snapshot, and it never fails the run (its time is
    // logged). The reader serializes that read with HDF5 users outside the coordinator: the
    // facade installs one that runs it under its review lanes' lock and session mutex
    // (review::ReviewJobs::runStatistics, #722), which defers it while a review job runs (the
    // numbers are then computed on demand when the instrument is idle). Without one nothing
    // serializes the read (a native shell's own HDF5 users), so the pass defers the file and
    // reads nothing; it is computed on demand. The reader is called with its own
    // lock held, so clearing it (nullptr, the facade's teardown) waits for a call in progress.
    using ClosedRunStatisticsReader = std::function<review::RunStatisticsResult(const std::string& path)>;
    void setClosedRunStatisticsReader(ClosedRunStatisticsReader reader);

    // The reconciled accounting of the last finalized run (#549), with that run's start
    // generation, or false before any run has finished. The UI uses it for the true admitted-frame
    // count and the individual loss counts, which the status's reason text only summarises.
    bool lastRunAccounting(recording::RecordingAccountingSnapshot& out, uint64_t& startGeneration) const;

    // Latest provisional KDE core contour record (JSON, frontend codec) that
    // the Monitoring view is showing. Accepted only while a run is Active;
    // cleared at Start; written to the run's file during finalization
    // (Hdf5Service::writeKdeLiveJson) as a copy of what was on screen. Never
    // blocks or fails the stop.
    void setLiveKdeCoreRecord(std::string json);

    void requestFlush();

    // Fatal save-error funnel (writer thread): marks the run Failed and
    // finalizes it so the file is closed and readable.
    void onFatalSaveError(const std::string& message);

    // Bounded, idempotent: finalizes an active run and joins the worker.
    // Called from AppBackend::shutdown() and the destructor.
    void shutdown();

    // Returns the frozen snapshot of the run that is active or was most
    // recently finalized; kept for callers that log the run identity. It no
    // longer changes state: finalization is owned by the coordinator.
    std::optional<RunConfigurationSnapshot> finish();

    // Lifecycle status (issue #372 G2/G3). The callback fires on every
    // transition, outside the coordinator mutex; consumers must not block
    // (same rule as the facade event sink).
    using StatusCallback = std::function<void(const ExperimentStatus&)>;
    void setStatusCallback(StatusCallback cb);
    ExperimentStatus status() const;

    ExperimentRunState state() const;
    /// The state without taking the coordinator mutex (safe under it and under the live mutex); a snapshot.
    ExperimentRunState stateNow() const { return state_.load(); }
    // Frozen snapshot of the active run (empty when Idle).
    std::optional<RunConfigurationSnapshot> activeRun() const;
    uint64_t readinessGeneration() const { return readinessGeneration_.load(); }

    // Fault sink: an unresolved lifecycle/save/data-integrity fault blocks
    // readiness until cleared.
    void reportUnresolvedFault(const std::string& code, const std::string& message);
    void clearUnresolvedFault();
    bool acknowledgeFault(uint64_t expectedRun, uint64_t expectedFaultRevision,
                          const std::string& expectedCode, const std::string& expectedMessage,
                          std::string& error);
    // Runs a config transaction while Start is excluded. Nested service setters
    // reuse the authorization of the enclosing idle transaction on this thread.
    // Workers use wait=false to avoid waiting on transactions that may join them.
    bool withIdleConfiguration(const std::function<void()>& transaction);
    bool withIdleConfiguration(const std::function<void()>& transaction, bool wait);
    // The same, for the live tuning of the cell acceptance and target group criteria (#866 MON-080;
    // the Qt Monitoring tune panel applies them during a run). Idle behaves exactly like
    // withIdleConfiguration (`midRun` false). While the run is Active the transaction also runs, with
    // `midRun` true, under the same coordinator lock, so it can neither overlap Start nor the
    // finalization: Stop's acceptance, the worker's switch to Stopping and the file's closing all
    // need that lock, and a run that has been asked to stop is refused here. The caller must change
    // nothing but the tune criteria and the provenance text (processingPatchIsTuneOnly); a
    // withIdleConfiguration nested in a midRun transaction is refused (the run is not idle).
    // Refused (false, transaction not run) when the experiment is Starting, Stopping or Failed, when
    // an Active run is being stopped, on a PL instrument and during a calibration method's run
    // (`refusal`, when given, then says why; it stays empty for the plain "not idle" cases): the PL
    // runs with the profile compiled at Start, so a host-side change during the run would only make
    // the file's recorded settings untrue; a calibration run derives its record at close from the
    // run's valid rows under the method and configuration identity of its Start, so rows validated
    // under changed criteria would be proposed under an identity that does not describe them.
    bool withTuneConfiguration(const std::function<void(bool midRun)>& transaction, std::string* refusal = nullptr);
    // The rule withTuneConfiguration applies to a run that is not Idle, so the table of refusals can
    // be tested without driving a run into every state: true only for an Active run that holds a
    // run (`hasRun`), has not been asked to stop, is processed on the host and is not a calibration
    // method's run (`calibrationRun`); `why` gets the reason for the cases that have one (an Active
    // run that is being stopped, a PL instrument, a calibration run).
    static bool midRunTuneAllowed(ExperimentRunState state, bool hasRun, bool stopping, bool hostProcessing,
                                  bool calibrationRun, std::string* why = nullptr);
    bool hasUnresolvedFault() const;

private:
    friend struct ExperimentConfigurationTestAccess;
    // Everything readiness depends on, in one comparable value.
    struct InvalidationKey {
        uint64_t captureGeneration{0};
        double runFps{0.0}, runExposureUs{0.0}; // the Run setting the camera runs at (PZ7035), so a change refreshes the readiness
        bool runPending{false};
        bool cameraReady{false};
        std::string cameraSource;
        bool cameraFallback{false};
        std::string deliveryMode;
        uint64_t processingConfigVersion{0};
        std::string configJsonSha256;
        std::string coreVersion;
        std::string coreSha256;
        bool corePinSatisfied{false};
        uint64_t backgroundGeneration{0};
        int roiX{0}, roiY{0}, roiW{0}, roiH{0};
        bool roiPending{false};
        double pixelToMicron{0.0};
        std::string outputPath;
        std::string profileId;
        std::string method; // methodInvalidationKey (#398 M2)
        std::string calibration; // CalibrationGateResult::invalidationKey (#716 A4a)
        bool faulted{false};
        uint64_t frameWidth{0}, frameHeight{0}, pixelFormat{0};
        bool frameGeometryKnown{false};
        uint64_t bufferBytes{0};
        size_t flushInterval{0};
        // #833: what the capacity gates read (the available RAM is not part of
        // the key: it changes all the time and only moves the plan's size).
        uint64_t bufferOverrideBytes{0};
        size_t invalidSampling{0};
        bool acquisitionKnown{false};
        int acqWidth{0}, acqHeight{0}, acqOffsetX{0}, acqOffsetY{0};
        double requestedFps{0.0}, maxFps{0.0}, targetFps{0.0};
        std::string profileWindow;
        uint64_t windowApplySequence{0};
        bool operator==(const InvalidationKey& o) const;
        bool operator!=(const InvalidationKey& o) const { return !(*this == o); }
    };
    InvalidationKey currentKeyLocked(const std::string& outputPath, const std::string& profileId) const;
    // The output path a client gave, as the run uses it (#762): ".h5" appended
    // when it has no HDF5 extension, then AppBackend::resolveClientPath (a
    // relative path lies under the storage root; one that leaves it is refused).
    // The one place a start path is accepted: readiness and Start both call it,
    // so the probes, the candidate, the snapshot, the reply and the status all
    // carry the resolved absolute path. `refusal` is set (and `path` is the
    // request) when it is refused; both are empty for an empty request.
    struct AcceptedOutputPath {
        std::string path;
        std::string refusal;
    };
    AcceptedOutputPath acceptOutputPath(const std::string& requested) const;
    // `outputRefusal` (from acceptOutputPath): storage.output fails with it and
    // nothing is probed.
    ExperimentReadinessSnapshot evaluateLocked(const std::string& outputPath,
                                               const std::string& profileId,
                                               const std::string& outputRefusal = {});
    RunConfigurationSnapshot candidateLocked(const std::string& outputPath,
                                             const std::string& profileId) const;
    // The `calibration.record` gate (#716 A4a) for the candidate: parses the applied method's
    // calibration members (memoized by the config hash), invalidates the applied record when the
    // known identity changed (a store append, once per change), and evaluates the method's policy.
    CalibrationGateResult calibrationGateLocked(const RunConfigurationSnapshot& candidate);
    // #833 capacity inputs. `measure`: run the destination write-rate probe
    // for `outputPath` when no measurement is cached (else cached only).
    CameraWindowGateInputs windowGateInputsLocked(const RunConfigurationSnapshot& candidate) const;
    // `useSessionRates`: take the requested and maximum rate from the running
    // session's camera report (false for an Overview session or another window).
    RecordingCapacityInputs capacityInputsLocked(const RunConfigurationSnapshot& candidate,
                                                 const std::string& outputPath, bool measure,
                                                 bool useSessionRates);
    // The running session acquires the experiment window (not an Overview).
    bool experimentSessionLocked(const RunConfigurationSnapshot& candidate) const;
    // A closed run of a calibration method (#716 A4a): derive its record from the run's own file
    // and append it to the store. Called by the finalization with the coordinator lock released
    // and the state still Stopping/Failed, so no review reader can open meanwhile (HDF5 is not
    // thread-safe, #722). Never fails the run.
    void deriveCalibrationRecord(const RunConfigurationSnapshot& run, const CalibrationMethodSpec& spec,
                                 const CalibrationIdentity& identity, bool finalizationOk);
    // Step 6b's run quick look (setClosedRunStatisticsReader). Lock released, never throws.
    void quickLookAtClose(const RunConfigurationSnapshot& run);
    ExperimentStatus snapshotLocked() const;
    // Publish the current snapshot: copies it, releases `lk`, invokes the
    // callback, re-acquires `lk`.
    void publishLocked(std::unique_lock<std::mutex>& lk, const char* message);
    void worker();
    // Runs the finalization sequence with `lk` released for the I/O and
    // re-acquired before the terminal state is written. `stopReason` is the
    // coordinator path that ended the run (recording::RunStopRecord), written
    // to the file as its stop record.
    void finalizeLocked(std::unique_lock<std::mutex>& lk, bool cancelled, bool failed,
                        const std::string& failMessage, const char* stopReason);

    AppBackend& backend_;
    mutable std::mutex mutex_;
    std::function<bool()> stageBusyProbe_; // under mutex_
    std::mutex closedRunStatisticsMutex_;
    ClosedRunStatisticsReader closedRunStatisticsReader_; // under closedRunStatisticsMutex_
    std::atomic<uint64_t> readinessGeneration_{0};
    InvalidationKey lastKey_;
    bool haveLastKey_{false};
    // Destination probes that passed for (output path, readiness generation) (#713).
    std::string storageProbePath_;
    uint64_t storageProbeGeneration_{0};
    bool storageWriteVerified_{false}; // the temp file of storage.output
    bool storageProbeOk_{false};       // the HDF5 round trip of storage.roundtrip
    std::string storageProbeReason_;
    // Destination write rates (#833, storage.throughput), per (directory,
    // saved frame size); under mutex_.
    WriteRateProbe writeRateProbe_;
    struct WriteRateEntry {
        WriteRateMeasurement measurement;
        // false: the measurement fails the floor stream. It stays for display
        // (the Overview panel) but the next measuring evaluation probes again (#841).
        bool settled{true};
    };
    std::map<std::string, WriteRateEntry> writeRates_;
    std::string lastWriteRateKey_;

    // Written under mutex_; atomic so start() can answer AlreadyActive without the lock (#595).
    std::atomic<ExperimentRunState> state_{ExperimentRunState::Idle};
    uint64_t startCounter_{0};
    std::optional<RunConfigurationSnapshot> activeRun_;
    std::optional<WallClock::Hold> wallHold_; // keeps the wall-clock offset fixed for the run (G14)
    std::string appVersion_{"unknown"};
    std::string buildId_;
    std::string os_;
    bool faultActive_{false};
    uint64_t faultRevision_{0};
    std::string faultCode_;
    std::string faultMessage_;
    // Terminal/lifecycle fields that outlive activeRun_ (reset on start).
    ExperimentStatus status_;
    recording::RecordingAccountingSnapshot lastAccounting_{};
    uint64_t lastAccountingGeneration_{0};
    bool haveLastAccounting_{false};
    mutable std::mutex callbackMutex_;
    StatusCallback statusCallback_;
    // publishLocked() invokes a copy of the callback outside every lock, so
    // replacing or clearing it must wait for invocations already in flight:
    // otherwise an owner that unregisters and is destroyed (BackendFacade at
    // shutdown) can still be called on the worker thread.
    std::condition_variable callbackIdle_;
    int callbacksInFlight_{0};
    // Worker thread (periodic flush + finalization). Guarded by mutex_.
    std::thread worker_;
    std::condition_variable workerCv_;
    bool workerExit_{false};
    bool flushRequested_{false};
    bool stopRequested_{false};
    bool cancelRequested_{false};
    bool fatalRequested_{false};
    std::string fatalMessage_;
    std::optional<RunConfigurationSnapshot> lastRun_;
    // Provisional KDE core record for the active run (guarded by mutex_).
    std::string liveKdeCoreJson_;
    // Method provenance memo (guarded by mutex_): readiness is polled, so the
    // registry snapshot copy and config canonicalization run only when an
    // input changes.
    struct MethodMemo {
        bool valid{false};
        std::string rawConfigSha256;
        uint64_t registryGeneration{0};
        std::string contextHash;
        std::string instrumentName;
        MethodProvenance method;
    };
    mutable MethodMemo methodMemo_;
    // The applied method's calibration members (#716 A4a), parsed once per config.json (guarded
    // by mutex_).
    struct CalibrationSpecMemo {
        bool valid{false};
        std::string rawConfigSha256;
        CalibrationMethodSpec spec;
        std::string error;
    };
    CalibrationSpecMemo calibrationSpecMemo_;
    // The calibration method of the active run and the identity at its Start (guarded by mutex_):
    // the finalization derives its record (#716 A4a). Empty for a measurement run.
    struct ActiveCalibration {
        CalibrationMethodSpec spec;
        CalibrationIdentity identity;
    };
    std::optional<ActiveCalibration> activeCalibration_;
    // Multi-image series runs force inline realtime processing; restored on
    // finalize (moved here from the Qt window).
    bool restoreRealtimeMode_{false};
    bool realtimeModeWasAsyncBatch_{false};
};

} // namespace backend::app
