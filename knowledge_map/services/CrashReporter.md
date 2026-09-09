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
- **Qt-free (epic #246):** the Qt log routing (`qInstallMessageHandler` →
  spdlog, criticals/fatals → Sentry) moved out of this backend service into
  the frontend `[[../frontend/System-Utilities]]` (`QtLogBridge`), installed
  from `main.cpp` after `init()`. It calls back into `captureMessage()`. The
  backend links no Qt.
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
  1. **Legacy recovery:** every earlier "assumed delivered" state —
     `.dmp.uploaded` (pre-#345), `.dmp.queued` and `.dmp.queued2` (the
     optimistic `sentry_capture_minidump` hand-off, 2026-08/09) — goes
     back to `.dmp` (sidecar likewise) and re-enters the queue. None of
     them ever carried delivery evidence; the bench PC held 99 such dumps
     that never reached the server
     (`docs/evidence/2026-09-08-crash-dump-review.md` §2).
  2. **Bounded retention:** removes the oldest files beyond
     `maxRetainedDumps` (default 50) per class: delivered dumps
     (`.dmp.sent` + `.dmp.rejected` together), pending `.dmp` files, and
     orphan `.json` sidecars / `.txt` notes with no matching dump —
     sidecars of pending dumps are never touched. Runs before the upload
     so the newest dumps are the ones that get uploaded.
  3. **Pending upload (2026-09-09, [[MinidumpUploader]]):** when a DSN is
     configured (parseable; Sentry does not even have to initialize), a
     background thread posts pending `.dmp` files **oldest first**, at
     most `maxUploadsPerStart` (10) per launch, straight to Sentry's
     `/api/<project>/minidump/` endpoint (WinHTTP on Windows, libcurl
     elsewhere) with the state sidecar as an attachment and the terminate
     `.txt` as `crash_message`. The disk queue is advanced only on the
     HTTP status: 2xx → `.dmp.sent`, permanent 4xx (not 408/429) →
     `.dmp.rejected`, anything else (offline, timeout, 5xx, 429) → the
     dump stays `.dmp` for the next launch. `shutdown()` stops the thread
     before `sentry_close()`; the dump in flight finishes (bounded by
     `uploadTimeoutMs`) and the first dump of a launch is always
     attempted, so even a seconds-long session drains one.

## Key APIs

```cpp
struct Config { dsn; release; environment; crashDir; databaseDir;
                installSignalHandlers; installTerminateHandler;
                uploadPendingOnStart; };
                tracesSampleRate; installSignalHandlers;
                installQtMessageHandler; installTerminateHandler;
                uploadPendingOnStart; maxRetainedDumps;
                maxUploadsPerStart; uploadTimeoutMs; shutdownTimeoutMs; };

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
  *.dmp.sent / *.json.sent / *.txt.sent   ← delivered (HTTP 2xx from the minidump endpoint)
  *.dmp.rejected (+ .json/.txt)           ← server refused permanently (4xx), never retried
  *.dmp.uploaded / .queued / .queued2     ← legacy suffixes (recovered to .dmp on next launch)
```

### File lifecycle

```
[crash] → .dmp + .json (+ .txt on the terminate path)
[next launch, DSN set] → MinidumpUploader POST (oldest first, ≤ maxUploadsPerStart)
                          2xx → .dmp.sent (+ .json.sent, .txt.sent)
                          4xx (not 408/429) → .dmp.rejected
                          offline / timeout / 5xx / 429 → stays .dmp, retried next launch
[next launch, no DSN] → .dmp + .json stay as-is (uploaded later)
[legacy recovery] → .dmp.uploaded / .queued / .queued2 → .dmp → (uploaded as above)
[retention cleanup] → oldest removed beyond maxRetainedDumps, per class:
                      delivered (.sent/.rejected), pending .dmp, orphan .json/.txt
```

### What a dump and its sidecar carry (2026-09-08)

The SIGSEGV/SIGFPE/SIGILL path recovers the `EXCEPTION_POINTERS` the MSVC
CRT publishes to signal handlers (`_pxcptinfoptrs`) and passes them to
`MiniDumpWriteDump`, so the dump's exception stream is the fault (code,
address, faulting thread), not the dump writer's breakpoint. Before this
every `-sigsegv.dmp` on the bench was unusable in `!analyze` (55 of them,
crash review 2026-09-08). The `.json` sidecar gains a `"crash"` object:
`code`, `address`, `module` + `module_offset`, `thread_id`, `access`
(read/write/execute) and `target` for access violations, and
`exe_build_id` — the PDB GUID+age read from the exe's CodeView debug
directory at `init()` (also logged). `init()` copies the exe's PDB, when
one sits next to it (dev builds), to `<crashDir>/../symbols/<build id>/`
so a dump from a bench binary stays symbolizable after the tree is
rebuilt: `cdbX64 -z <dmp> -y <that folder> -c "!analyze -v; ~*k"`.
Guard: `backend.crash_reporter_segv` (null write on a worker thread; the
parent reads the dump's exception stream back with dbghelp and checks the
sidecar and the kept PDB).

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
- **Never advance the crash queue without delivery evidence.** Dumps are
  renamed only on an HTTP status from [[MinidumpUploader]]. The previous
  scheme handed dumps to `sentry_capture_minidump` and renamed them
  `.queued` on the assumption of delivery; sentry-native's transport
  gives no per-envelope signal, drops a failed send silently, keeps the
  queue only in memory, and on a clean close flushes for 2 s before
  dumping the rest into the `.run` dir for a *single* re-send on the
  next launch (`sentry__process_old_runs` deletes the files before
  sending). On the bench (16 MB backlog, ~0.85 MB/s uplink, 20-second
  sessions, exit-time crashes) that lost every report for a month.
- **sentry-native's own queue is still used for live events** (messages,
  sessions, performance transactions); `shutdownTimeoutMs` (5 s, was the
  2 s library default) bounds its flush at `shutdown()`.
- **Do not block the UI on uploads.** The uploader is a background thread
  started at the end of `init()`; `shutdown()` joins it (the dump in
  flight finishes, the rest wait for the next launch).
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
- **Windows `-terminate` artifacts need the vectored exception handler
  (`cxxThrowVectoredHandler`), not just `std::set_terminate`.** Three
  MSVC facts, none of which apply to glibc (so Linux `backend-ci.yml`
  never saw the problem; only `build-windows.yml`'s CTest did):
  1. **`set_terminate` is per-thread** and not inherited by new threads
     (MS docs: "each new thread needs to install its own terminate
     function"). `init()` can only install `terminateHandler` on the
     calling thread, so an exception escaping a worker thread hit the CRT
     default — a plain `abort()` — and only ever produced `-sigabrt.*`
     via the SIGABRT fallback.
  2. **MSVC's `std::thread` entry shim is `noexcept`**, so an escaping
     exception makes the frame handler call `terminate()` during the SEH
     *search* phase. `sehHandler` (`SetUnhandledExceptionFilter`) is never
     reached on that path — editing it cannot fix this.
  3. **`std::current_exception()` is null in a terminate handler for an
     uncaught exception** on MSVC (the CRT only sets it while a catch
     block runs), so even a working handler had no `what()` for the
     `.txt` note.
  Fix: a first-chance `AddVectoredExceptionHandler(1, …)` that runs on the
  throwing thread for every C++ `throw` (`0xE06D7363`) process-wide and
  (a) lazily calls `std::set_terminate(terminateHandler)` on that thread,
  (b) copies `what()` into a `thread_local` `LastThrowRecord` by walking
  the exception record's ThrowInfo → CatchableTypeArray for a
  `std::exception` subobject (x64/ARM64 image-relative layout, SEH-guarded
  so a malformed record can't turn a throw into a crash). It always
  returns `EXCEPTION_CONTINUE_SEARCH`, so it never interferes with catch
  dispatch or Crashpad. `terminateHandler` uses the record when
  `current_exception()` is null; it is best-effort (the last throw on
  that thread). `sehHandler` additionally forwards a C++ exception that
  does reach the top-level filter (a thread whose entry point is not
  `noexcept`) to `std::terminate()`, replacing the CRT's displaced
  `__CxxUnhandledExceptionFilter`. The handler is removed in
  `shutdown()`.
- Building with `MIB_USE_SENTRY=OFF` (or with no DSN) keeps the local
  minidump path active — useful for offline / air-gapped deployments.
- **`sentry_capture_minidump` vs `sentry_capture_event`:** The upload
  path uses `sentry_capture_minidump(path)` which attaches the actual
  `.dmp` binary to the Sentry event. The old `sentry_capture_event`
  approach only sent a message event without the minidump attachment,
  making stack-based grouping and symbolication impossible.
