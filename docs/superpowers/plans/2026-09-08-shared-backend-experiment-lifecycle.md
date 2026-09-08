# Shared Backend Experiment Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `ExperimentCoordinator` own the whole run lifecycle (readiness, start, periodic flush, stop, finalization, terminal accounting outcome) so the Qt window and the bridge facade are two clients of one backend, then pin that surface in the bridge contract.

**Architecture:** The existing reliability `ExperimentCoordinator` (`include/backend/app/ExperimentCoordinator.h`) gains a worker thread, a status snapshot + callback, `requestStop()`, `onFatalSaveError()`, and `shutdown()`. `BackendFacade` gains `ExperimentCommand`, `ExperimentStatusEvent`, and status/readiness pulls. `MainWindow` stops touching `Hdf5Service` for experiments and renders coordinator status. A second PR on the bridge branch appends the enums to `bridge-contract.json`, pins them in the shim/Rust/TS mirrors, and maps the bridge experiment commands onto the facade.

**Tech Stack:** C++17, CMake/Conan (Windows VS2022 Release preset `windows-default`; CTest labels), spdlog, HDF5, Qt 6.7 (frontend only), Rust `cxx` bridge + Tauri v2 (contract PR).

Spec: `docs/superpowers/specs/2026-09-08-shared-backend-experiment-lifecycle-design.md`.

## Global Constraints

- Every code change lands with its vault note in the same commit (`knowledge_map/Vault-Maintenance.md`); run `python scripts/check_docs.py` before each commit.
- `spdlog` for logging; never `std::cout` in app code. Headers mirror sources under `include/`.
- Tests are bare `main()` executables using `tests/support/assert.h` (`MIB_REQUIRE`, `MIB_EXPECT`, `mib::test::exitCode()`), with a `mib::test::Watchdog` and no naked `join()`/`wait()`.
- Enum values that the bridge contract pins are append-only: `experiment_states` `Idle=0, Starting=1, Active=2, Stopping=3, Failed=4`; `command_types.Experiment=6`; `event_kinds.ExperimentStatus=8`.
- Status callbacks are invoked outside the coordinator mutex and consumers must not block (same rule as the facade event sink).
- Build/test on this bench: `cmake --build build --config Release --target <t> --parallel 16` from `D:\Developer\mib-studio-qt` (Git Bash: `export MSYS_NO_PATHCONV=1`), `ctest --test-dir build -C Release -R <name> --output-on-failure`.
- Commit trailer on every commit:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_019vVkC94Ra2KmwF6q93FewB`.

---

## File map

| File | Responsibility after this plan |
|---|---|
| `include/backend/app/ExperimentCoordinator.h` / `src/backend/app/ExperimentCoordinator.cpp` | Lifecycle authority: readiness, start, periodic flush, stop, finalization, status, faults, shutdown |
| `include/backend/app/ExperimentReadiness.h` | Unchanged types plus `ExperimentStopOutcome`, `ExperimentStatus` (new, next to the other public run types) |
| `src/backend/app/AppBackend.cpp` | Wires fatal save errors into the coordinator; `shutdown()` calls `experimentCoordinator_->shutdown()` |
| `include/backend/app/BackendFacade.h` / `src/backend/app/BackendFacade.cpp` | `ExperimentCommand`, `ExperimentStatusEvent`, `fetchExperimentReadiness/Status` |
| `src/frontend/core/MainWindow.cpp` / `include/frontend/core/MainWindow.h` | Client only: start/stop requests, status rendering, dialogs |
| `tests/backend/experiment_readiness_test.cpp` | + stop/finalize/fault/shutdown cases |
| `tests/backend/backend_facade_boundary_test.cpp` | + experiment command round trip |
| `knowledge_map/architecture/ExperimentCoordinator.md`, `knowledge_map/architecture/AppBackend.md`, `knowledge_map/frontend/MainWindow.md`, `knowledge_map/current-state/Recent-Work.md`, `docs/exec-plans/active/2026-09-08-agent-a-handoff-372.md` | Vault + handoff |
| Bridge branch (`D:\Developer\mib-studio-qt-agentb`, `agent-b/windows-native-bridge`): `crates/mib-bridge/contract/bridge-contract.json`, `crates/mib-bridge/src/shim.cpp`, `crates/mib-bridge/src/lib.rs`, `crates/mib-bridge/tests/contract.rs`, `desktop/src/bridgeContract.ts` (generated), `desktop/src-tauri/src/frame_packet_contract.rs` (generated) | Contract hardening (Task 7) |

---

### Task 1: Coordinator public types and status snapshot

**Files:**
- Modify: `include/backend/app/ExperimentReadiness.h` (append after `ExperimentStartResult`, ~line 153)
- Modify: `include/backend/app/ExperimentCoordinator.h`
- Modify: `src/backend/app/ExperimentCoordinator.cpp`
- Modify: `src/frontend/core/MainWindow.cpp` (rename `ExperimentRunState::Running` uses)
- Test: `tests/backend/experiment_readiness_test.cpp`

**Interfaces:**
- Produces: `enum class ExperimentRunState { Idle = 0, Starting = 1, Active = 2, Stopping = 3, Failed = 4 }`; `enum class ExperimentStopOutcome { Accepted, NotActive, Busy }`; `struct ExperimentStatus` (fields below); `using StatusCallback = std::function<void(const ExperimentStatus&)>`; `ExperimentStatus ExperimentCoordinator::status() const`; `void setStatusCallback(StatusCallback)`.

- [ ] **Step 1: Write the failing test** — append a new scenario block to `tests/backend/experiment_readiness_test.cpp` just before the final `return mib::test::exitCode();` (the file already has `backend`, `proc`, `startCapture(backend)`, `wd`, `td` in scope; copy the "concurrent start" block's setup for the output path):

```cpp
    // ---- status snapshot + callback (shared-backend lifecycle, task 1) ----
    {
        wd.mark("status snapshot");
        auto& coordinator = backend.experiment();
        const auto idle = coordinator.status();
        MIB_EXPECT(idle.state == backend::app::ExperimentRunState::Idle, "idle before start");
        MIB_EXPECT(!idle.terminal && idle.completion == backend::recording::RunCompletionState::Unknown,
                   "no completion before a run");

        std::vector<backend::app::ExperimentRunState> seen;
        std::mutex seenMutex;
        coordinator.setStatusCallback([&](const backend::app::ExperimentStatus& s) {
            std::lock_guard<std::mutex> lk(seenMutex);
            seen.push_back(s.state);
        });

        MIB_REQUIRE(startCapture(backend), "capture running for status test");
        const auto out = (td.path() / "status_run.h5").string();
        const auto r = coordinator.evaluateReadiness(out);
        MIB_REQUIRE(r.ready, "ready for status test");
        backend::app::ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        const auto started = coordinator.start(req);
        MIB_REQUIRE(started.started(), "started: " + started.message);
        const auto active = coordinator.status();
        MIB_EXPECT(active.state == backend::app::ExperimentRunState::Active, "Active after start");
        MIB_EXPECT(active.startGeneration == started.run.startGeneration, "status carries the run identity");
        MIB_EXPECT(active.outputPath == started.run.outputPath, "status carries the output path");
        {
            std::lock_guard<std::mutex> lk(seenMutex);
            MIB_EXPECT(!seen.empty() && seen.back() == backend::app::ExperimentRunState::Active,
                       "callback observed Active");
        }
        coordinator.setStatusCallback({});
        // Leave the run active; the next task's scenario stops it.
        MIB_REQUIRE(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted ||
                    true, "placeholder until task 2 (compiles only)");
    }
```

  Remove the last `MIB_REQUIRE(... || true ...)` line before committing Task 1; it is only there to force the compile error listed in Step 2.

- [ ] **Step 2: Run the test to verify it fails** — `cmake --build build --config Release --target experiment_readiness_test --parallel 16`; expected: compile errors `'status': is not a member of 'backend::app::ExperimentCoordinator'`, `'ExperimentStatus' is not a member`, `'Active' is not a member of 'backend::app::ExperimentRunState'`.

- [ ] **Step 3: Add the types** to `include/backend/app/ExperimentReadiness.h` after `ExperimentStartResult`:

```cpp
// Typed outcome of requestStop().
enum class ExperimentStopOutcome { Accepted, NotActive, Busy };
inline const char* toString(ExperimentStopOutcome o)
{
    switch (o) {
    case ExperimentStopOutcome::Accepted: return "accepted";
    case ExperimentStopOutcome::NotActive: return "notActive";
    case ExperimentStopOutcome::Busy: return "busy";
    }
    return "unknown";
}

// Pullable lifecycle snapshot; also delivered through the status callback on
// every transition. Qt-free by design so the bridge can carry it verbatim.
struct ExperimentStatus {
    ExperimentRunState state{ExperimentRunState::Idle};
    uint64_t startGeneration{0};
    uint64_t readinessGeneration{0};
    uint64_t captureGeneration{0};
    std::string outputPath;
    uint64_t startWallClockNs{0};
    uint64_t endWallClockNs{0};
    uint64_t validBuffered{0};
    uint64_t invalidBuffered{0};
    uint64_t persistenceAdmitted{0};
    uint64_t persistenceCommitted{0};
    uint64_t persistenceFailed{0};
    bool flushing{false};
    bool cancelled{false};
    bool terminal{false};        // finalization finished (Idle or Failed)
    bool finalizationOk{false};  // every finalize step succeeded
    backend::recording::RunCompletionState completion{backend::recording::RunCompletionState::Unknown};
    std::string completionReason;
    std::string faultCode;
    std::string faultMessage;
    std::string message;         // last human-readable transition note
};
```

  `ExperimentReadiness.h` must `#include "backend/recording/RecordingAccounting.h"` for `RunCompletionState` (add it next to the existing includes). Move `enum class ExperimentRunState` from `ExperimentCoordinator.h` into `ExperimentReadiness.h` (above `ExperimentStatus`) with the pinned values and rename `Running` to `Active`, add `Failed`:

```cpp
// Contract-pinned (bridge-contract.json experiment_states); append only.
enum class ExperimentRunState { Idle = 0, Starting = 1, Active = 2, Stopping = 3, Failed = 4 };
inline const char* toString(ExperimentRunState s)
{
    switch (s) {
    case ExperimentRunState::Idle: return "idle";
    case ExperimentRunState::Starting: return "starting";
    case ExperimentRunState::Active: return "active";
    case ExperimentRunState::Stopping: return "stopping";
    case ExperimentRunState::Failed: return "failed";
    }
    return "unknown";
}
```

- [ ] **Step 4: Coordinator declarations** in `ExperimentCoordinator.h` public section:

```cpp
    using StatusCallback = std::function<void(const ExperimentStatus&)>;
    // Fired on every transition, outside the coordinator mutex; consumers
    // must not block (same rule as the facade event sink).
    void setStatusCallback(StatusCallback cb);
    ExperimentStatus status() const;
    // Task 2 adds requestStop / onFatalSaveError / shutdown.
```

  Private members: `std::mutex callbackMutex_; StatusCallback statusCallback_; ExperimentStatus status_;` plus helpers `ExperimentStatus snapshotLocked() const;` and `void publishLocked(std::unique_lock<std::mutex>& lk, const char* message);` (copies the snapshot, sets `message`, unlocks, calls the callback, relocks).

- [ ] **Step 5: Implement** in `ExperimentCoordinator.cpp`:

```cpp
void ExperimentCoordinator::setStatusCallback(StatusCallback cb)
{
    std::lock_guard<std::mutex> lk(callbackMutex_);
    statusCallback_ = std::move(cb);
}

ExperimentStatus ExperimentCoordinator::snapshotLocked() const
{
    ExperimentStatus s = status_;
    s.state = state_;
    if (activeRun_) {
        s.startGeneration = activeRun_->startGeneration;
        s.readinessGeneration = activeRun_->readinessGeneration;
        s.captureGeneration = activeRun_->captureGeneration;
        s.outputPath = activeRun_->outputPath;
        s.startWallClockNs = activeRun_->startWallClockNs;
    }
    const auto counts = backend_.processing().getBufferedFrameCounts();
    s.validBuffered = counts.valid;
    s.invalidBuffered = counts.invalid;
    const auto acc = backend_.processing().experimentAccountingSnapshot();
    s.persistenceAdmitted = acc.persistenceAdmitted;
    s.persistenceCommitted = acc.persistenceCommitted;
    s.persistenceFailed = acc.persistenceFailed;
    s.faultCode = faultActive_ ? faultCode_ : std::string{};
    s.faultMessage = faultActive_ ? faultMessage_ : std::string{};
    return s;
}

ExperimentStatus ExperimentCoordinator::status() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return snapshotLocked();
}

void ExperimentCoordinator::publishLocked(std::unique_lock<std::mutex>& lk, const char* message)
{
    ExperimentStatus s = snapshotLocked();
    s.message = message ? message : "";
    status_.message = s.message;
    StatusCallback cb;
    {
        std::lock_guard<std::mutex> clk(callbackMutex_);
        cb = statusCallback_;
    }
    lk.unlock();
    if (cb) cb(s);
    lk.lock();
}
```

  `start()` uses `std::unique_lock` already; publish `Starting` right after `state_ = ExperimentRunState::Starting;` and `Active` after `state_ = ExperimentRunState::Active;` (rename from `Running`). Also update `start()`'s `AlreadyActive` test to compare with `Active`. Reset `status_ = {}` at the top of a successful start (before publishing `Starting`) so a new run does not inherit the previous terminal fields. `faultMessage_` is a new `std::string` member set in `reportUnresolvedFault`.

- [ ] **Step 6: Fix the renames** — `grep -rn "ExperimentRunState::Running" src include tests` and replace with `Active` (expected: `MainWindow.cpp` and `ExperimentCoordinator.cpp` only).

- [ ] **Step 7: Build and run** — `cmake --build build --config Release --target experiment_readiness_test mib_studio_qt --parallel 16` then `ctest --test-dir build -C Release -R backend.experiment_readiness --output-on-failure`; expected: PASS (delete the placeholder line first).

- [ ] **Step 8: Commit**

```bash
git add include/backend/app/ExperimentReadiness.h include/backend/app/ExperimentCoordinator.h src/backend/app/ExperimentCoordinator.cpp src/frontend/core/MainWindow.cpp tests/backend/experiment_readiness_test.cpp
git commit -m "feat(experiment): coordinator status snapshot, status callback, contract-pinned run states"
```

---

### Task 2: Worker-owned periodic flush, requestStop finalization, fatal funnel, shutdown

**Files:**
- Modify: `include/backend/app/ExperimentCoordinator.h`, `src/backend/app/ExperimentCoordinator.cpp`
- Modify: `src/backend/app/AppBackend.cpp` (fatal funnel at ~line 222; `shutdown()` at ~line 147)
- Test: `tests/backend/experiment_readiness_test.cpp`

**Interfaces:**
- Consumes: Task 1 types.
- Produces: `ExperimentStopOutcome requestStop(bool cancelled);` `void onFatalSaveError(const std::string& message);` `void shutdown();` and the finalization sequence from the spec. `ExperimentStatus::terminal == true` with `completion` set once finalization ends.

- [ ] **Step 1: Write the failing tests** — replace the placeholder block from Task 1 with:

```cpp
    // ---- backend-owned finalization (task 2) ----
    {
        wd.mark("finalize complete");
        auto& coordinator = backend.experiment();
        std::optional<backend::app::ExperimentStatus> terminal;
        std::mutex tMutex;
        coordinator.setStatusCallback([&](const backend::app::ExperimentStatus& s) {
            if (s.terminal) { std::lock_guard<std::mutex> lk(tMutex); terminal = s; }
        });
        MIB_REQUIRE(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::NotActive ||
                    coordinator.status().state == backend::app::ExperimentRunState::Active,
                    "NotActive when idle");
        MIB_REQUIRE(startCapture(backend), "capture running");
        const auto out = (td.path() / "finalize_run.h5").string();
        auto r = coordinator.evaluateReadiness(out);
        backend::app::ExperimentStartRequest req;
        req.outputPath = out; req.readinessGeneration = r.generation;
        MIB_REQUIRE(coordinator.start(req).started(), "start");
        // Let frames accumulate so a remainder exists at stop.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Busy ||
                   coordinator.status().terminal, "second stop while stopping is Busy");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            { std::lock_guard<std::mutex> lk(tMutex); if (terminal) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard<std::mutex> lk(tMutex);
        MIB_REQUIRE(terminal.has_value(), "terminal status published");
        MIB_EXPECT(terminal->finalizationOk, "finalization ok");
        MIB_EXPECT(terminal->state == backend::app::ExperimentRunState::Idle, "Idle after a clean stop");
        MIB_EXPECT(terminal->completion == backend::recording::RunCompletionState::Complete ||
                   terminal->completion == backend::recording::RunCompletionState::IntentionallyPartial,
                   std::string("clean run completion: ") + terminal->completionReason);
        MIB_EXPECT(terminal->persistenceCommitted == terminal->persistenceAdmitted, "stop-time remainder credited");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "file closed by the coordinator");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::NotActive, "NotActive after finalize");
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(out), "finalized file reloads");
        backend::recording::RecordingAccountingSnapshot back;
        MIB_REQUIRE(reader.readRunAccounting(back), "accounting persisted by the coordinator");
        MIB_EXPECT(back.reconciled, "persisted accounting reconciles");
        coordinator.setStatusCallback({});
    }
    {
        wd.mark("fatal save error");
        auto& coordinator = backend.experiment();
        MIB_REQUIRE(startCapture(backend), "capture running");
        const auto out = (td.path() / "fatal_run.h5").string();
        auto r = coordinator.evaluateReadiness(out);
        backend::app::ExperimentStartRequest req;
        req.outputPath = out; req.readinessGeneration = r.generation;
        MIB_REQUIRE(coordinator.start(req).started(), "start");
        coordinator.onFatalSaveError("injected writer failure");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!coordinator.status().terminal && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto s = coordinator.status();
        MIB_REQUIRE(s.terminal, "fatal error finalizes");
        MIB_EXPECT(s.state == backend::app::ExperimentRunState::Failed, "Failed after a fatal save error");
        MIB_EXPECT(s.completion == backend::recording::RunCompletionState::Failed, "completion Failed");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "file still closed after failure");
        MIB_EXPECT(coordinator.hasUnresolvedFault(), "fault latched for the next preflight");
        coordinator.clearUnresolvedFault();
        backend::services::Hdf5Service reader;
        MIB_EXPECT(reader.loadFile(out), "failed run's file is readable");
    }
    {
        wd.mark("shutdown while active");
        auto& coordinator = backend.experiment();
        MIB_REQUIRE(startCapture(backend), "capture running");
        const auto out = (td.path() / "shutdown_run.h5").string();
        auto r = coordinator.evaluateReadiness(out);
        backend::app::ExperimentStartRequest req;
        req.outputPath = out; req.readinessGeneration = r.generation;
        MIB_REQUIRE(coordinator.start(req).started(), "start");
        coordinator.shutdown();
        const auto s = coordinator.status();
        MIB_EXPECT(s.terminal && !backend.hdf5().isFileOpen(), "shutdown finalizes and closes the file");
        coordinator.shutdown(); // idempotent
    }
```

- [ ] **Step 2: Run to verify it fails** — build `experiment_readiness_test`; expected compile errors for `requestStop`, `onFatalSaveError`, `shutdown`.

- [ ] **Step 3: Declarations** (public):

```cpp
    ExperimentStopOutcome requestStop(bool cancelled);
    void onFatalSaveError(const std::string& message);
    void shutdown();
    ~ExperimentCoordinator();
```

  Private: `void worker(); void finalizeLocked(std::unique_lock<std::mutex>& lk, bool cancelled, bool failed, const std::string& failMessage); std::thread worker_; std::condition_variable workerCv_; bool stopRequested_{false}; bool cancelRequested_{false}; bool fatalRequested_{false}; std::string fatalMessage_; bool workerExit_{false}; bool restoreRealtimeMode_{false}; int realtimeModeBefore_{0};`. Add `#include <condition_variable>` and `#include <thread>`.

- [ ] **Step 4: Worker and stop** in `ExperimentCoordinator.cpp`:

```cpp
ExperimentCoordinator::~ExperimentCoordinator() { shutdown(); }

ExperimentStopOutcome ExperimentCoordinator::requestStop(bool cancelled)
{
    std::unique_lock<std::mutex> lk(mutex_);
    if (state_ == ExperimentRunState::Stopping) return ExperimentStopOutcome::Busy;
    if (state_ != ExperimentRunState::Active && state_ != ExperimentRunState::Failed) return ExperimentStopOutcome::NotActive;
    if (state_ == ExperimentRunState::Failed && !activeRun_) return ExperimentStopOutcome::NotActive;
    stopRequested_ = true;
    cancelRequested_ = cancelled;
    workerCv_.notify_all();
    return ExperimentStopOutcome::Accepted;
}

void ExperimentCoordinator::onFatalSaveError(const std::string& message)
{
    std::unique_lock<std::mutex> lk(mutex_);
    if (state_ != ExperimentRunState::Active) return;
    fatalRequested_ = true;
    fatalMessage_ = message;
    stopRequested_ = true;
    workerCv_.notify_all();
}

void ExperimentCoordinator::worker()
{
    std::unique_lock<std::mutex> lk(mutex_);
    while (!workerExit_) {
        workerCv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return stopRequested_ || workerExit_; });
        if (workerExit_ && !activeRun_) break;
        if (!stopRequested_ && state_ == ExperimentRunState::Active) {
            auto& proc = backend_.processing();
            if (proc.getBufferedFrameCounts().total() >= proc.getFlushInterval()) {
                lk.unlock();
                const size_t n = proc.flushBufferedFrames(backend_.hdf5());
                if (n > 0) SPDLOG_DEBUG("ExperimentCoordinator: periodic flush submitted {} frames", n);
                lk.lock();
            }
            continue;
        }
        if (stopRequested_) {
            const bool failed = fatalRequested_;
            const std::string msg = fatalMessage_;
            const bool cancelled = cancelRequested_;
            stopRequested_ = false; fatalRequested_ = false; cancelRequested_ = false;
            finalizeLocked(lk, cancelled, failed, msg);
        }
    }
}
```

  `start()`: after entering `Active`, if `!worker_.joinable()` set `workerExit_ = false` and `worker_ = std::thread([this] { worker(); });`. Also record `realtimeModeBefore_`/`restoreRealtimeMode_` when the frozen candidate has `multi_image_enabled` in the processing config and the mode is not Inline (mirror `MainWindow::onStartExperiment` lines 1410-1420: switch to Inline and remember to restore).

- [ ] **Step 5: Finalization**:

```cpp
void ExperimentCoordinator::finalizeLocked(std::unique_lock<std::mutex>& lk, bool cancelled, bool failed,
                                           const std::string& failMessage)
{
    state_ = failed ? ExperimentRunState::Failed : ExperimentRunState::Stopping;
    status_.cancelled = cancelled;
    publishLocked(lk, failed ? "fatal save error; finalizing" : "stopping");
    auto& proc = backend_.processing();
    auto& hdf5 = backend_.hdf5();
    const auto run = activeRun_;
    lk.unlock();

    bool ok = !failed;
    const bool fileOpen = hdf5.isFileOpen();
    if (fileOpen) {
        proc.flushBufferedFrames(hdf5);
        if (!proc.finishFlush()) ok = false;
    }
    proc.endExperiment();
    proc.resetRealtimeMetrics();
    const auto remainder = proc.getBufferedFrameCounts();
    if (fileOpen && remainder.total() > 0) {
        proc.flushBufferedFrames(hdf5);
        if (!proc.finishFlush()) ok = false;
    }
    const uint64_t endNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    auto accounting = proc.experimentAccountingSnapshot();
    if (fileOpen) {
        if (!hdf5.flush()) SPDLOG_WARN("ExperimentCoordinator: H5Fflush before metadata failed");
        const auto cfg = proc.getProcessingConfig();
        const auto roi = proc.getRealtimeRoi();
        cv::Mat bg = proc.getRealtimeBackgroundGray();
        const auto core = proc.activeProcessingCoreIdentity();
        const bool metaOk = hdf5.writeExperimentInfo(run ? run->startWallClockNs : 0, endNs,
                                                     remainder.valid, remainder.invalid, cfg, roi,
                                                     bg.empty() ? nullptr : &bg, &core);
        if (!metaOk) ok = false;
        if (metaOk && !hdf5.writeRunAccounting(accounting)) ok = false;
        const std::string cfgJson = backend_.getLastConfigJson();
        if (metaOk && !cfgJson.empty()) hdf5.writeConfigJson(cfgJson);
        hdf5.closeFile();
    }
    if (restoreRealtimeMode_) {
        proc.setRealtimeProcessingMode(realtimeModeBefore_ == 1
            ? services::ProcessingService::RealtimeProcessingMode::AsyncBatch
            : services::ProcessingService::RealtimeProcessingMode::Inline);
        restoreRealtimeMode_ = false;
    }

    lk.lock();
    activeRun_.reset();
    status_.endWallClockNs = endNs;
    status_.terminal = true;
    status_.finalizationOk = ok;
    status_.completion = failed ? recording::RunCompletionState::Failed : accounting.completion;
    status_.completionReason = failed ? failMessage : accounting.completionReason;
    if (!ok && !failed) {
        faultActive_ = true; faultCode_ = "experiment.flushFailed";
        faultMessage_ = "a save error occurred while finalizing experiment data";
    }
    if (failed) {
        faultActive_ = true; faultCode_ = "experiment.saveFailed"; faultMessage_ = failMessage;
    }
    state_ = (failed || !ok) ? ExperimentRunState::Failed : ExperimentRunState::Idle;
    publishLocked(lk, ok ? "finalized" : "finalized with errors");
    if (state_ == ExperimentRunState::Failed) state_ = ExperimentRunState::Idle; // next start allowed once the fault is cleared
}

void ExperimentCoordinator::shutdown()
{
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (state_ == ExperimentRunState::Active) { stopRequested_ = true; }
        workerExit_ = true;
        workerCv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}
```

  Note on `Failed`: the terminal status carries `state == Failed` (that is what the callback and the pull show for that transition); the coordinator then rests in `Idle` with the fault latched, so the readiness gate `lifecycle.fault` blocks the next start until `clearUnresolvedFault()`. `finish()` stays for compatibility and now only returns the run snapshot without changing state (finalization already reset it); update its doc comment.

- [ ] **Step 6: Wire AppBackend** — in `AppBackend.cpp` ~line 222 replace the flush error lambda body with `experimentCoordinator_ ? experimentCoordinator_->onFatalSaveError(msg) : void(); reportFatalSaveError(msg);` (the coordinator is constructed at ~line 234, so move `experimentCoordinator_ = std::make_unique<...>(*this);` above the `setFlushErrorCallback` call). In `AppBackend::shutdown()` add, before `captureService_->stop()`: `if (experimentCoordinator_) experimentCoordinator_->shutdown();`.

- [ ] **Step 7: Build and run** — `ctest --test-dir build -C Release -R "backend.experiment_readiness|backend.lifecycle_smoke|backend.smoke" --output-on-failure`; expected: PASS. Also run `ctest --test-dir build -C Release -R "processing.experiment_accounting|recording." --output-on-failure` (no regressions).

- [ ] **Step 8: Vault** — `knowledge_map/architecture/ExperimentCoordinator.md`: replace the "finish() leaves finalization to the caller" text with the worker/finalize sequence (steps 1–8 of the spec) and the concurrency rules; `knowledge_map/architecture/AppBackend.md`: note the fatal-save funnel and shutdown order. `python scripts/check_docs.py`.

- [ ] **Step 9: Commit**

```bash
git add include/backend/app/ExperimentCoordinator.h src/backend/app/ExperimentCoordinator.cpp src/backend/app/AppBackend.cpp tests/backend/experiment_readiness_test.cpp knowledge_map/architecture/ExperimentCoordinator.md knowledge_map/architecture/AppBackend.md
git commit -m "feat(experiment): coordinator-owned periodic flush, asynchronous stop finalization, fatal funnel, shutdown"
```

---

### Task 3: Facade experiment commands, status pull, and status event

**Files:**
- Modify: `include/backend/app/BackendFacade.h`, `src/backend/app/BackendFacade.cpp`
- Test: `tests/backend/backend_facade_boundary_test.cpp`

**Interfaces:**
- Produces: `BackendCommandType::Experiment` (value 6: declare `Experiment = 6` explicitly, and pin the earlier values `Camera = 0 … PlaybackSeek = 4`; value 5 is reserved for the migration branch's `Operation`), `enum class ExperimentCommandAction { EvaluateReadiness = 0, Start = 1, Stop = 2, Status = 3 }`, `struct ExperimentCommand`, `struct ExperimentStatusEvent { backend::app::ExperimentStatus status; }` appended to the `BackendEvent` variant, `BackendCommandResult::experimentStartOutcome` / `experimentStopOutcome` (`std::optional<>`), `bool fetchExperimentReadiness(backend::app::ExperimentReadinessSnapshot&, const std::string& outputPath = {}, const std::string& profileId = {}) const;`, `bool fetchExperimentStatus(backend::app::ExperimentStatus&) const;`.

- [ ] **Step 1: Write the failing test** — in `backend_facade_boundary_test.cpp`, after the existing `stopRecording` dispatch and before `loadRecording`, insert (the file has `facade`, `events`, `dataDir`, a running mock capture):

```cpp
    // Experiment lifecycle through the facade only (shared backend, #372 G2/G3).
    {
        bridge::ExperimentCommand evaluate;
        evaluate.action = bridge::ExperimentCommandAction::EvaluateReadiness;
        evaluate.outputPath = (dataDir / "facade_run.h5").string();
        const auto r = facade.dispatch(evaluate);
        if (!r.ok) { std::cerr << "readiness evaluation failed: " << r.message << "\n"; return 1; }
        backend::app::ExperimentReadinessSnapshot readiness;
        if (!facade.fetchExperimentReadiness(readiness, evaluate.outputPath) || !readiness.ready) {
            std::cerr << "facade readiness should be ready with a running mock camera\n"; return 1;
        }
        bridge::ExperimentCommand start;
        start.action = bridge::ExperimentCommandAction::Start;
        start.outputPath = evaluate.outputPath;
        start.readinessGeneration = readiness.generation;
        const auto s = facade.dispatch(start);
        if (!s.ok || !s.experimentStartOutcome || *s.experimentStartOutcome != backend::app::ExperimentStartOutcome::Started) {
            std::cerr << "facade start failed: " << s.message << "\n"; return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        backend::app::ExperimentStatus status;
        if (!facade.fetchExperimentStatus(status) || status.state != backend::app::ExperimentRunState::Active) {
            std::cerr << "facade status should be Active\n"; return 1;
        }
        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        const auto st = facade.dispatch(stop);
        if (!st.ok || !st.experimentStopOutcome || *st.experimentStopOutcome != backend::app::ExperimentStopOutcome::Accepted) {
            std::cerr << "facade stop not accepted: " << st.message << "\n"; return 1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        bool sawTerminal = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (facade.fetchExperimentStatus(status) && status.terminal) { sawTerminal = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!sawTerminal || !status.finalizationOk) { std::cerr << "facade stop did not finalize\n"; return 1; }
        size_t experimentEvents = 0;
        for (const auto& e : events) if (std::holds_alternative<bridge::ExperimentStatusEvent>(e)) ++experimentEvents;
        if (experimentEvents < 3) { std::cerr << "expected Starting/Active/Stopping/terminal status events, got " << experimentEvents << "\n"; return 1; }
    }
```

  (`events` is filled by the sink under a mutex in the existing test; if the vector is guarded, take the same lock when counting.)

- [ ] **Step 2: Run to verify it fails** — build `backend_facade_boundary_test`; expected compile errors on `ExperimentCommand`, `experimentStartOutcome`, `fetchExperimentStatus`.

- [ ] **Step 3: Declarations** in `BackendFacade.h` (`#include "backend/app/ExperimentReadiness.h"`):

```cpp
    enum class BackendCommandType { Camera = 0, Recording = 1, ProcessingSettings = 2, RecordingLoad = 3, PlaybackSeek = 4, Experiment = 6 };
    enum class ExperimentCommandAction { EvaluateReadiness = 0, Start = 1, Stop = 2, Status = 3 };
    struct ExperimentCommand
    {
        ExperimentCommandAction action{ExperimentCommandAction::Status};
        std::string outputPath;
        std::uint64_t readinessGeneration{0};
        std::string profileId;
        bool acknowledgeLatestFrameDrops{false};
        bool cancelled{false};
    };
    // add ExperimentCommand to the BackendCommand variant (last)
    struct BackendCommandResult
    {
        bool ok{false};
        BackendCommandType command{BackendCommandType::Camera};
        std::string message;
        std::optional<backend::app::ExperimentStartOutcome> experimentStartOutcome;
        std::optional<backend::app::ExperimentStopOutcome> experimentStopOutcome;
    };
    struct ExperimentStatusEvent { backend::app::ExperimentStatus status; };
    // add ExperimentStatusEvent to the BackendEvent variant (last)
    bool fetchExperimentReadiness(backend::app::ExperimentReadinessSnapshot &out, const std::string &outputPath = {}, const std::string &profileId = {}) const;
    bool fetchExperimentStatus(backend::app::ExperimentStatus &out) const;
  private:
    BackendCommandResult handleExperimentCommand(const ExperimentCommand &command);
```

- [ ] **Step 4: Implementation** in `BackendFacade.cpp`: extend the `commandTypeOf` visitor and the `dispatch` visitor with `ExperimentCommand`; add

```cpp
    BackendCommandResult BackendFacade::handleExperimentCommand(const ExperimentCommand &command)
    {
        if (!initialized_) return lifecycleError(BackendCommandType::Experiment, "backend not initialized");
        BackendCommandResult result;
        result.command = BackendCommandType::Experiment;
        auto &coordinator = backend_.experiment();
        switch (command.action)
        {
        case ExperimentCommandAction::EvaluateReadiness: {
            const auto r = coordinator.evaluateReadiness(command.outputPath, command.profileId);
            result.ok = true;
            result.message = r.ready ? "ready" : "not ready";
            return result;
        }
        case ExperimentCommandAction::Start: {
            backend::app::ExperimentStartRequest req;
            req.outputPath = command.outputPath;
            req.readinessGeneration = command.readinessGeneration;
            req.profileId = command.profileId;
            req.acknowledgeLatestFrameDrops = command.acknowledgeLatestFrameDrops;
            const auto r = coordinator.start(req);
            result.experimentStartOutcome = r.outcome;
            result.ok = r.started();
            result.message = r.message;
            return result;
        }
        case ExperimentCommandAction::Stop: {
            const auto o = coordinator.requestStop(command.cancelled);
            result.experimentStopOutcome = o;
            result.ok = o == backend::app::ExperimentStopOutcome::Accepted;
            result.message = backend::app::toString(o);
            return result;
        }
        case ExperimentCommandAction::Status:
            result.ok = true;
            result.message = backend::app::toString(coordinator.status().state);
            return result;
        }
        return lifecycleError(BackendCommandType::Experiment, "unknown experiment action");
    }

    bool BackendFacade::fetchExperimentReadiness(backend::app::ExperimentReadinessSnapshot &out, const std::string &outputPath, const std::string &profileId) const
    {
        if (!initialized_) return false;
        out = backend_.experiment().evaluateReadiness(outputPath, profileId);
        return true;
    }

    bool BackendFacade::fetchExperimentStatus(backend::app::ExperimentStatus &out) const
    {
        if (!initialized_) return false;
        out = backend_.experiment().status();
        return true;
    }
```

  In `initialize()` after the backend initializes: `backend_.experiment().setStatusCallback([this](const backend::app::ExperimentStatus &s) { emitEvent(ExperimentStatusEvent{s}); });` and in `shutdown()` clear it with `setStatusCallback({})` before the backend shutdown. (`evaluateReadiness` is non-const on the coordinator; keep `fetchExperimentReadiness` const on the facade by calling through the non-const `backend_` reference the facade already holds.)

- [ ] **Step 5: Build and run** — `ctest --test-dir build -C Release -R "backend.facade_boundary|backend.experiment_readiness" --output-on-failure`; expected PASS.

- [ ] **Step 6: Vault** — the facade note (`knowledge_map/architecture/Data-Flow.md` or wherever `BackendFacade` is documented; `grep -rl BackendFacade knowledge_map`): add the experiment command/event surface and the pinned values.

- [ ] **Step 7: Commit**

```bash
git add include/backend/app/BackendFacade.h src/backend/app/BackendFacade.cpp tests/backend/backend_facade_boundary_test.cpp knowledge_map
git commit -m "feat(facade): experiment commands, readiness/status pulls, ExperimentStatus events over the shared coordinator"
```

---

### Task 4: MainWindow becomes a client of the coordinator

**Files:**
- Modify: `src/frontend/core/MainWindow.cpp` (`onStartExperiment` ~1321–1485, `onStopExperiment` ~1503–1566, `finishStopExperiment` ~1568–1770, periodic flush ~1878–1893, `closeEvent` ~660–675), `include/frontend/core/MainWindow.h`
- Test: frontend lane (`ctest -R "^frontend\."`), `backend.kin6_mib_app_capture_proof`, plus the bench mock run in Task 5

**Interfaces:**
- Consumes: `ExperimentCoordinator::requestStop`, `status`, `setStatusCallback`, `ExperimentStatus`.
- Produces: `private slot void onExperimentStatus(const backend::app::ExperimentStatus& status);` (queued to the GUI thread).

- [ ] **Step 1: Register the callback** — in the `MainWindow` constructor after `backend_` is initialized and `runStatusModel_` exists: `qRegisterMetaType<backend::app::ExperimentStatus>("backend::app::ExperimentStatus");` (declare `Q_DECLARE_METATYPE(backend::app::ExperimentStatus)` in `MainWindow.h`), then `backend_.experiment().setStatusCallback([this](const backend::app::ExperimentStatus& s) { QMetaObject::invokeMethod(this, [this, s] { onExperimentStatus(s); }, Qt::QueuedConnection); });` and in the destructor `backend_.experiment().setStatusCallback({});` before anything else is torn down.

- [ ] **Step 2: Replace `onStopExperiment`**:

```cpp
void MainWindow::onStopExperiment()
{
    if (!experimentActive_) {
        QMessageBox::information(this, tr("Experiment"), tr("No experiment is currently running"));
        return;
    }
    if (stopInProgress_) return; // one finalization only
    const auto outcome = backend_.experiment().requestStop(false);
    if (outcome != backend::app::ExperimentStopOutcome::Accepted) {
        SPDLOG_WARN("MainWindow: stop refused ({})", backend::app::toString(outcome));
        return;
    }
    stopInProgress_ = true;
    runStatusModel_->setPhase(frontend::RunPhase::Stopping, runOperationId_);
    updateExperimentButtonStates();
    statusLabel_->setText(tr("Stopping experiment…"));
}
```

- [ ] **Step 3: Replace `finishStopExperiment(bool)` with `onExperimentStatus`** — keep only presentation: on `Stopping` set the Saving phase text; on `terminal`: if `!status.finalizationOk` raise the `save.flush` alert, latch failure, and show the "Save Error" box; if `completion` is `Failed`/`IncompleteLoss` raise the `run.accounting` alert and latch; build the accounting text from `status` (`persistenceCommitted/Admitted/Failed`, `completionReason`) and show it with `QMessageBox::warning(this, tr("Experiment Accounting"), text)` last; then `experimentActive_ = false; stopInProgress_ = false; runStatusModel_->setPhase(frontend::RunPhase::Complete, runOperationId_); updateExperimentButtonStates(); statusLabel_->setText(...)`. Delete `finalizeWatcher_`, `finalizeHandled_`, `restoreRealtimeModeIfNeeded()` and `restoreRealtimeModeAfterExperiment_`/`realtimeModeBeforeExperiment_` (the coordinator restores the mode now; `onStartExperiment` keeps the multi-image information box but no longer switches the mode itself), and every `backend_.hdf5()` use in the stop path. Keep `experimentStartTimeNs_` (runtime label) fed from `status.startWallClockNs` on `Active`.

- [ ] **Step 4: Remove the window's periodic flush** — delete the `flushWatcher_` block at ~1878–1893 and the `waitForFinished` in the old stop path; keep `flushInProgress_` only if the diagnostics struct at ~1850 needs it (set it from `status.flushing`). Also `closeEvent` (~660–675): replace the finalize watcher wait with `backend_.experiment().shutdown();` before `AppBackend::shutdown()` is invoked (or rely on `AppBackend::shutdown()` from Task 2 and just drop the wait).

- [ ] **Step 5: Build and run** — `cmake --build build --config Release --parallel 16` then `ctest --test-dir build -C Release -R "^frontend\.|backend.kin6_mib_app_capture_proof|backend.facade_boundary|backend.experiment_readiness" --output-on-failure`; expected PASS (`frontend.ui_layout` may need one rerun; it is intermittent).

- [ ] **Step 6: Vault** — `knowledge_map/frontend/MainWindow.md`: the window is a client; list what moved to the coordinator; keep the "no modal before file close" gotcha as "dialogs only on the terminal status". `Recent-Work.md` entry dated 2026-09-08. `check_docs.py`.

- [ ] **Step 7: Commit**

```bash
git add src/frontend/core/MainWindow.cpp include/frontend/core/MainWindow.h knowledge_map/frontend/MainWindow.md knowledge_map/current-state/Recent-Work.md
git commit -m "refactor(ui): MainWindow renders coordinator status; experiment finalization no longer lives in the window"
```

---

### Task 5: Full verification, handoff update, push

**Files:**
- Modify: `docs/exec-plans/active/2026-09-08-agent-a-handoff-372.md` (G2, G3 closed; G5 partial)
- Bench artifacts under `D:\data\bench-validation\`

- [ ] **Step 1: Lanes** — `ctest --preset windows-test`, `ctest --test-dir build -C Release -L integration`, hardware lane with `MIB_TEST_CAMERA=1 MIB_CAMERA_MODE=hardware MIB_TEST_NANOPOSITIONER_PORT=6 MIB_TEST_EGRABBER_SCRIPT=<repo>\resources\defaults\egrabberConfig.js` → `ctest --preset windows-hardware-test`. Expected: all pass (exporter soak requires PySide6 + opencv on the bench, already installed).

- [ ] **Step 2: Bench mock recording** — `python <scratchpad>\prepare_hf_run.py` (tuned config + median background frames), launch `build\Release\mib_studio_qt.exe` with `MIB_CAMERA_MODE=mock MIB_MOCK_CAMERA_DIR=D:\data\bench-validation\hf-mock MIB_MOCK_CAMERA_INTERVAL_MS=1 MIB_MOCK_CAMERA_LOOP=true`, Overview → Experiment → enable Auto background → Start (`D:\data\bench-validation\shared-backend-run.h5`) → 2 min → Stop; then `python <scratchpad>\reconcile_accounting.py D:/data/bench-validation/shared-backend-run.h5`. Expected: derived state Complete, `persistence_pending_at_stop_frames = 0`, `finishStopExperiment`-equivalent log line from the coordinator shows the finalize timing, file closed before any dialog. Restore `build\include\config.json` from `config.json.pre-hf-soak.bak`.

- [ ] **Step 3: Handoff doc** — mark G2 and G3 done with the commit SHAs, G5 "status pull available, watermark/retained outcomes still open"; `check_docs.py`.

- [ ] **Step 4: Commit and push**

```bash
git add docs/exec-plans/active/2026-09-08-agent-a-handoff-372.md
git commit -m "docs(#372): handoff manifest — G2/G3 closed by the shared coordinator and facade experiment surface"
git push origin claude/host-sdk-reliability-qt-ui-g03ubd
gh pr comment 379 --body "Shared-backend experiment lifecycle landed on this branch: coordinator-owned finalization + facade experiment commands (spec docs/superpowers/specs/2026-09-08-shared-backend-experiment-lifecycle-design.md). Lanes green; bench mock recording reconciles Complete through the coordinator."
```

---

### Task 6: Integration branch for the bridge (converged backend under the bridge)

**Files (worktree `D:\Developer\mib-studio-qt-agentb`, new local branch `agent-b/shared-backend-integration` from `agent-b/windows-native-bridge`):**
- Merge: `claude/host-sdk-reliability-qt-ui-g03ubd` (after Task 5)
- Resolve: `include/backend/app/ExperimentCoordinator.h`, `src/backend/app/ExperimentCoordinator.cpp` → take the reliability versions wholesale (the migration coordinator is retired here); `include/backend/app/BackendFacade.h` / `.cpp` → keep the reliability `ExperimentCommand`/`ExperimentStatusEvent` shape and port the migration facade's remaining command handlers (`Operation`, `Monitoring`, `Trigger`, `Review`, `Pump`, `Autofocus`) onto it with their pinned values (`Operation = 5`, `Monitoring = 7`, `Trigger = 8`, `Review = 9`, `Pump = 10`, `Autofocus = 11`); `src/backend/services/{CameraControlService,CrashReporter,SyringePumpService,TriggerService}.cpp`, `include/backend/services/SyringePumpService.h`, `include/backend/camera/mindvision/MindVisionConfig.h`, `include/backend/camera/mock/MockCamera.h` → reliability side wins; vault/CI files → keep both sides.
- Modify: `crates/mib-bridge/src/shim.cpp`, `crates/mib-bridge/src/lib.rs` (experiment commands call `ExperimentCommand`; `experiment_status` reads `fetchExperimentStatus`; `ExperimentStatus` event carries the new fields)

- [ ] **Step 1: Merge** — `git checkout -b agent-b/shared-backend-integration && git merge --no-commit --no-ff origin/claude/host-sdk-reliability-qt-ui-g03ubd`; resolve per the rules above; `git commit`.
- [ ] **Step 2: Build the backend archives** — `conan install . -of build --build=missing -s build_type=Release -r conancenter`, `cmake --preset windows-default -DMIB_MINDVISION_SDK_ROOT="C:/Program Files (x86)/MindVision"`, `cmake --build build --config Release --target mib_backend mib_processing mib_backend_smoke_test --parallel 16`, then `python tools/gen_bridge_link_manifest.py`.
- [ ] **Step 3: Port the shim** — every `experiment_*` bridge function dispatches `ExperimentCommand`; the `BridgeExperimentStatus` struct gains `start_generation`, `readiness_generation`, `capture_generation`, `terminal`, `finalization_ok`, `completion` (u8 pinned to `run_completion_states`), `completion_reason`, `fault_code`, `fault_message`; the event sink maps `ExperimentStatusEvent` to `BridgeEventKind::ExperimentStatus` (8).
- [ ] **Step 4: Run** — `cargo test --no-fail-fast -- --test-threads=1` in `crates/mib-bridge` with `MIB_BRIDGE_NO_CMAKE=1` and the manifest's `runtime_dirs` on `PATH`; expected: `experiment_lifecycle_end_to_end` passes against the converged coordinator (it drives start → status → stop → load). Fix the test's expectations where they assumed the migration `Status` fields, keeping the assertions' intent (terminal status observed, file reloads).
- [ ] **Step 5: Commit** on the integration branch: `git commit -am "merge: reliability backend under the bridge; retire the migration ExperimentCoordinator"`.

---

### Task 7: Contract hardening (bridge branch)

**Files:**
- Modify: `crates/mib-bridge/contract/bridge-contract.json` (`abi_version` 12 → 13; append `experiment_command_actions`, `experiment_start_outcomes`, `experiment_stop_outcomes`, `run_completion_states`, `readiness_gate_statuses`)
- Modify: `crates/mib-bridge/src/shim.cpp` (`static_assert`s), `crates/mib-bridge/tests/contract.rs` (`rust_enums_match_contract_json` covers the new groups), `scripts/gen_bridge_contract.py` (`ENUM_GROUPS` gains the five names), regenerate `desktop/src/bridgeContract.ts` and `desktop/src-tauri/src/frame_packet_contract.rs`
- Modify: `desktop/src/` event adapter (`ExperimentStatus` typed JSON with decimal-string u64s for the three generations, following #375's `event-json-v1.md`), `docs/architecture/event-json-v1.md`

**Interfaces:**
- Consumes: C++ enums from Tasks 1 and 3.
- Produces: contract groups with these exact values:

```json
"experiment_command_actions": { "EvaluateReadiness": 0, "Start": 1, "Stop": 2, "Status": 3 },
"experiment_start_outcomes": { "Started": 0, "NotReady": 1, "StaleReadiness": 2, "AlreadyActive": 3, "StorageFailed": 4, "ProvenanceFailed": 5, "Busy": 6 },
"experiment_stop_outcomes": { "Accepted": 0, "NotActive": 1, "Busy": 2 },
"run_completion_states": { "Complete": 0, "IntentionallyPartial": 1, "IncompleteLoss": 2, "Failed": 3, "Unknown": 4 },
"readiness_gate_statuses": { "Pass": 0, "Warn": 1, "Fail": 2, "Unavailable": 3, "NotRequired": 4 }
```

  and, in `ExperimentReadiness.h`, explicit `= N` initializers on `ExperimentStartOutcome`, `ExperimentStopOutcome`, `GateStatus` and on `RunCompletionState` in `RecordingAccounting.h` matching these values (append-only from now on).

- [ ] **Step 1: Write the failing contract test** — in `contract.rs::rust_enums_match_contract_json`, add assertions that the JSON has the five groups with the values above (mirroring the existing pattern for `experiment_states`).
- [ ] **Step 2: Run** `cargo test --test contract rust_enums_match_contract_json`; expected FAIL (`missing key`).
- [ ] **Step 3: Add the groups** to the JSON, bump `abi_version` to 13 and the shim's `kBridgeAbiVersion`, add `static_assert(static_cast<int>(backend::app::ExperimentStartOutcome::Busy) == 6, "contract drift")` etc. for every value, update `ENUM_GROUPS`, run `python scripts/gen_bridge_contract.py` (writes TS/Rust), then `python scripts/gen_bridge_contract.py --check` → exit 0.
- [ ] **Step 4: Typed status payload** — extend the JSON event payload for `ExperimentStatus` (decimal-string `startGeneration`, `readinessGeneration`, `captureGeneration`; `completion` as the contract name; `terminal`, `finalizationOk`, `completionReason`, `faultCode`); add a golden fixture through the existing C++→Rust→JSON→TS fixture pipeline (`contract-fixtures` feature) and a `desktop/src` test that decodes it (`npm test`).
- [ ] **Step 5: Run everything** — `cargo test` (bridge and `desktop/src-tauri`), `npm run build`, `npm test`, `python scripts/gen_bridge_contract.py --check`, `python scripts/check_docs.py`; expected all green on Windows.
- [ ] **Step 6: Docs and commit** — `knowledge_map/architecture/Rust-Bridge.md` and `Desktop-Shell.md`: the experiment surface and ABI 13; `docs/architecture/event-json-v1.md`: the status payload. Commit: `git commit -am "contract(bridge): ABI 13 — experiment command/outcome, run completion and gate status groups; typed ExperimentStatus payload"`. Do not push `agent-b/*` branches without the maintainer's go-ahead; open the PR against `agent-b/event-contracts` when told.

---

## Self-review

- Spec coverage: architecture (Tasks 1–2), coordinator API (1–2), finalization sequence and concurrency (2), facade surface (3), Qt client (4), contract hardening (6–7), tests (each task + 5), documentation (each task + 5). The Qt periodic flush removal is in Task 4 Step 4. Realtime-mode restore moves in Task 2 Step 4/5 and is removed from the window in Task 4 Step 3.
- Placeholders: none; the only intentionally temporary line (Task 1 Step 1) is called out and removed within the task.
- Type consistency: `ExperimentStatus`, `ExperimentStopOutcome`, `ExperimentRunState::Active/Failed`, `requestStop(bool)`, `fetchExperimentStatus`, `fetchExperimentReadiness`, `ExperimentStatusEvent{status}` are used identically across Tasks 1–7.
