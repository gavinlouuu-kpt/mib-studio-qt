# CrashReporter

> Installs process-level crash handlers, writes minidumps + JSON state
> sidecars on unrecoverable failures, and (optionally) forwards events to
> Sentry. Initialized in `main()` BEFORE Logger and AppBackend so that
> early-startup crashes are still captured.

**Source:** `src/backend/services/CrashReporter.cpp`,
`include/backend/services/CrashReporter.h`
**Related:** [[../diagnostics/CrashStateMirror]],
[[../conventions/Logging]], [[../architecture/AppBackend]]

## Responsibility

- Installs (local-only mode, i.e. when Sentry is not active):
  - Windows `SetUnhandledExceptionFilter` → `MiniDumpWriteDump` (always
    available via `dbghelp.lib`).
  - `std::signal` handlers for SIGSEGV / SIGABRT / SIGFPE / SIGILL.
  - `std::set_terminate` for uncaught C++ exceptions.
  - `qInstallMessageHandler` to route Qt warnings/criticals into spdlog
    and forward fatal Qt messages as Sentry events.
- When Sentry is active (`isSentryActive() == true`):
  - The SEH filter and SIGSEGV/SIGFPE/SIGILL handlers are **not**
    installed — Crashpad owns native fault capture. The
    `sentry_options_set_on_crash` callback writes the JSON state sidecar
    when Crashpad catches a crash.
  - A **SIGABRT handler is still installed** as a local fallback:
    Crashpad only intercepts SEH/native faults, so a CRT `abort()` would
    otherwise produce no dump at all.
  - `std::set_terminate` and `qInstallMessageHandler` are still
    installed (they handle C++ exceptions and Qt fatals that Crashpad
    does not intercept).
- On crash: writes `{timestamp}-pid{N}-{reason}.dmp` (Windows) and a
  `.json` sidecar containing the current
  [[../diagnostics/CrashStateMirror]] snapshot under
  `%LOCALAPPDATA%/MIB_Studio_Qt/crashes/`.
- On startup (when `uploadPendingOnStart` is true):
  1. **Legacy recovery:** scans for `.dmp.uploaded` files (the old
     suffix from before issue #345) and renames them back to `.dmp`
     (plus matching `.json.uploaded` → `.json`) for re-submission.
  2. **Pending upload:** only when Sentry actually initialized
     (`isSentryActive()`), scans for `.dmp` files and submits each via
     `sentry_capture_minidump(path)`, which attaches the actual minidump
     binary to the Sentry event. Each dump's JSON sidecar is loaded and
     attached as a `state_snapshot` extra, then cleaned up after
     submission to prevent state leakage between dumps. Submitted dumps
     are renamed to `.dmp.queued` (not `.uploaded`). When Sentry is
     inactive (no DSN / init failure / built without Sentry), pending
     dumps are left untouched so a later launch can submit them.
     A `.dmp.queued` still on disk after `queuedRetryAfterDays` (default
     7; 0 disables) is re-submitted exactly once — tagged
     `crash_recovery: queued_retry` — and renamed to the terminal
     `.dmp.queued2`. This covers the transport's send-failure loss
     window (see Gotchas).
  3. **Bounded retention:** removes the oldest files beyond
     `maxRetainedDumps` (default 50) per class: queued dumps
     (`.dmp.queued` + `.dmp.queued2` together), never-submitted pending
     `.dmp` files (local-only installs), and orphan `.json` sidecars
     (terminate / on_crash / diagnostic files with no matching dump) —
     sidecars of dumps still on disk are never touched.

## Key APIs

```cpp
struct Config { dsn; release; environment; crashDir; databaseDir;
                tracesSampleRate; installSignalHandlers;
                installQtMessageHandler; installTerminateHandler;
                uploadPendingOnStart; maxRetainedDumps;
                queuedRetryAfterDays; };

static bool init(const Config& cfg);
static void shutdown();
static bool isInitialized();
static bool isSentryActive();
static void setTag(string_view k, string_view v);
static void setContextJson(string_view name, string_view json);
static void breadcrumb(string_view category, string_view msg,
                       string_view jsonData = {});
static void registerStateMirror(StateSnapshotFn);
static void captureMessage(string_view);
static void captureException(string_view);
static void capturePerformanceTransaction(string_view name,
    string_view op, double durationMs, string_view jsonData = {});
static bool writeDiagnosticSnapshot(string_view reason);
```

Public free-form decoration calls (`setTag`, `breadcrumb`) are no-ops when
Sentry is not compiled in — they remain safe to sprinkle through services.

## Handler ownership

When `isSentryActive()` is true (Sentry initialized successfully with a
DSN), CrashReporter does **not** install its own SEH filter or
SIGSEGV/SIGFPE/SIGILL handlers — Crashpad's handlers take precedence for
native crash capture. It **does** keep a SIGABRT handler, because
Crashpad never sees a CRT `abort()`. The `on_crash` callback
(`sentry_options_set_on_crash`) is registered so that CrashReporter can
still write the JSON state sidecar at crash time; the callback guards
against reentrancy, snapshots the state mirror exactly once (skipping it
when the mutex is contended rather than calling it unlocked), and sets
`state_snapshot` on the event itself rather than the scope (scope
mutations this late may not reach the crashpad-uploaded event).

When Sentry is not active (no DSN, `MIB_USE_SENTRY=OFF`, or init
failure), CrashReporter falls back to its own handlers for local
minidump capture.

The `std::terminate` handler writes its own minidump **unconditionally**
(issue #347): Crashpad never sees `std::terminate` — the `abort()` at the
end of the handler is intercepted by the SIGABRT fallback, which `_Exit`s
while `handlingCrash` is already set, so no other layer would produce a
dump for this path. The handler also writes the JSON sidecar and, when
terminate was reached via an unhandled exception, a `.txt` with the
exception's `what()`. The dump is submitted through the pending-upload
path on the next launch.

## DSN configuration

- Read from `MIB_SENTRY_DSN` env var at process start.
- Empty DSN → local-only mode: minidumps still written to disk, but never
  uploaded.
- Override environment label with `MIB_CRASH_ENV` (defaults to
  `production` for Release / `development` for Debug).
- Override performance transaction sampling with
  `MIB_SENTRY_TRACES_SAMPLE_RATE` (`0.0` to `1.0`; defaults are `0.20`
  for Release and `1.0` for Debug).

## Performance Monitoring

CrashReporter enables Sentry Performance transactions when Sentry is
configured. The current instrumentation covers:

- `experiment.stop` (`ui.action`) — total time spent stopping/saving an
  experiment.
- `hdf5.append_frames` (`hdf5.write`) — HDF5 append duration with valid,
  invalid, and multi-image series counts/timings in `perf_data`.
- `hdf5.close_file` (`hdf5.close`) — HDF5 close/flush duration.
- `playback.degraded` (`ui.render`) — throttled to at most once per minute
  when display FPS drops below 30, average latency exceeds 250 ms, dropped
  frames are detected, or overlay compute exceeds 30 ms.

In Sentry, look under **Performance** or filter transactions by
`release:mib_studio_qt@<version>` and `environment:production`.

### How the DSN reaches production installs

The release pipeline injects the DSN at three layers:

1. **CMake** — `cmake -DMIB_SENTRY_DSN=...` is set by the
   `Build Windows` workflow (from the `SENTRY_DSN` repo secret) when
   running the `CMake configure` step.
2. **InnoSetup** — CMake forwards `MIB_SENTRY_DSN` to ISCC via
   `/DSentryDSN=...`. The `[Registry]` section in
   `resources/installers/mib-studio-qt.iss` writes a system-wide
   `HKLM\…\Environment\MIB_SENTRY_DSN` value so every process spawned
   after install picks it up.
3. **Runtime** — `main.cpp` reads `MIB_SENTRY_DSN` (and the optional
   `MIB_CRASH_ENV`) via `qgetenv` and passes them into
   `CrashReporter::init`.

Operator setup (org slug, auth token, self-hosted URL) is documented in
[`docs/howto/sentry-setup.md`](../../docs/howto/sentry-setup.md).

## Crash artifacts

```
%LOCALAPPDATA%/MIB_Studio_Qt/crashes/
  20260522T143015-pid12345-seh.dmp        ← Windows minidump
  20260522T143015-pid12345-seh.json       ← state snapshot
  20260522T143015-pid12345-sigsegv.json   ← (signal-handler path, no dmp on non-Win)
  20260522T143015-pid12345-terminate.json ← std::terminate path (+ .dmp + .txt)
  20260522T143015-pid12345-exception.json ← non-fatal captureException()
  *.dmp.queued                            ← submitted to Sentry transport queue
  *.dmp.queued2                           ← re-submitted once after going stale (terminal)
  *.dmp.uploaded                          ← legacy suffix (recovered to .dmp on next launch)
```

### File lifecycle

```
[crash] → .dmp + .json (+ .txt on the terminate path)
[next launch, Sentry active] → sentry_capture_minidump() → .dmp.queued + .json.queued
[next launch, Sentry inactive] → .dmp + .json stay as-is (submitted later)
[stale .queued > queuedRetryAfterDays] → re-submitted once → .dmp.queued2 (terminal)
[legacy recovery] → .dmp.uploaded → .dmp → (re-submitted as above)
[retention cleanup] → oldest removed beyond maxRetainedDumps, per class:
                      queued (.queued/.queued2), pending .dmp, orphan .json
```

## Symbolication

Minidumps are useless without matching PDB. The CMake config emits
`mib_studio_qt.pdb` next to the `.exe` for Release builds (via `/Zi` +
`/DEBUG /OPT:REF /OPT:ICF`).

The `Build Windows` GitHub Actions workflow runs
`sentry-cli debug-files upload --include-sources build\Release` on
every release/beta build, so symbols are pushed automatically when
`SENTRY_AUTH_TOKEN` is present. The workflow then verifies the uploaded
symbols with `sentry-cli debug-files check` and creates a Sentry
release named `mib_studio_qt@<version>`, matching the `release` field
set at runtime by `main.cpp`.

For manual / hotfix uploads outside CI:

```powershell
$env:SENTRY_AUTH_TOKEN = "sntrys_..."
$env:SENTRY_URL = "https://sentry.yofo.bio"      # omit for sentry.io
$env:SENTRY_ORG = "sentry"
$env:SENTRY_PROJECT = "mib-studio-qt"

sentry-cli debug-files upload --include-sources build\Release
sentry-cli debug-files check build\Release\mib_studio_qt.pdb
sentry-cli releases new "mib_studio_qt@$version"
sentry-cli releases finalize "mib_studio_qt@$version"
```

## Gotchas

- **Crashpad needs `crashpad_handler.exe` next to the EXE.** The CMake
  post-build copy step handles this when `MIB_USE_SENTRY=ON`. Without it
  Sentry silently falls back to in-process capture and loses dumps from
  non-recoverable crashes (heap corruption, stack overflow).
- **Do not install a custom SEH filter or fault-signal handlers when
  Sentry is active.** They overwrite Crashpad's handlers and prevent
  minidump capture. The `init()` code gates SEH + SIGSEGV/SIGFPE/SIGILL
  installation behind `!isSentryActive()`; only the SIGABRT fallback
  (which Crashpad cannot see) stays installed in both modes.
- **Pending dumps are only renamed `.queued` after a real submission.**
  `uploadPendingCrashes` is gated on `isSentryActive()` — with Sentry
  inactive, `sentry_capture_minidump` would be a no-op and renaming
  would mark never-sent dumps as queued, letting retention destroy
  them. Note 0.7.20's `sentry_capture_minidump` returns `void`, so
  per-capture success cannot be verified beyond the active check.
- **The transport is only partially durable** (verified against the
  pinned sentry-native 0.7.20 source): envelopes still *waiting* in the
  bgworker queue at shutdown are dumped to the database `.run` folder
  (`sentry__transport_dump_queue`) and re-sent on a later launch
  (`sentry__process_old_runs`); an envelope whose send attempt *fails*
  (e.g. offline) is freed without any retry. The stale-`.queued`
  one-shot retry exists to cover exactly that loss window; `.queued2`
  is terminal so retries can never loop.
- **State snapshot isolation.** Each pending dump's extras
  (`state_snapshot`, `original_dump_file`) are set before
  `sentry_capture_minidump` and removed immediately after, so one dump's
  state never leaks into the next event.
- The signal handler intentionally re-raises the signal with `SIG_DFL`
  so debuggers and Windows Error Reporting still see the fault.
- `registerStateMirror` MUST point to a function that does not allocate
  unbounded memory or take locks held by the crashing thread. The
  [[../diagnostics/CrashStateMirror]] uses atomics + `try_lock` to
  satisfy this.
- Worker-thread exception handling is intentionally NOT hardened in this
  change (per `task/2026-05-22-crash-monitoring.md`). Crashpad only
  catches SEH/native faults — but since issue #347, a C++ exception that
  escapes a worker thread entry point reaches the terminate handler,
  which leaves a `.dmp` + `.json` + `.txt` and gets the event to Sentry
  on the next launch.
- **`sehHandler` must translate the MSVC C++-exception SEH code
  (`0xE06D7363`) into an explicit `std::terminate()` call.** Installing
  `SetUnhandledExceptionFilter(sehHandler)` replaces the CRT's own
  top-level filter — the one that normally recognizes this code and
  calls `std::terminate()` on our behalf. Without redoing that
  translation, a C++ exception that escapes every frame (e.g. off a
  worker thread) is intercepted by `sehHandler` first: it wrote generic
  `-seh.dmp`/`.json` files and returned `EXCEPTION_CONTINUE_SEARCH`,
  after which Windows tore the process down directly — `std::terminate`
  (and therefore `terminateHandler`'s `-terminate.json/.txt/.dmp` +
  exception message) was never reached. This was caught by
  `backend.crash_reporter_terminate` failing only in the Windows CI lane
  (`build-windows.yml`'s CTest step), never in Linux `backend-ci.yml`,
  since glibc's unwind calls `std::terminate` directly with no SEH
  translation step to clobber.
- Building with `MIB_USE_SENTRY=OFF` (or with no DSN) keeps the local
  minidump path active — useful for offline / air-gapped deployments.
- **`sentry_capture_minidump` vs `sentry_capture_event`:** The upload
  path uses `sentry_capture_minidump(path)` which attaches the actual
  `.dmp` binary to the Sentry event. The old `sentry_capture_event`
  approach only sent a message event without the minidump attachment,
  making stack-based grouping and symbolication impossible.
