# Logging

> All runtime logging goes through spdlog. Configured once by
> `backend::services::Logger::init(path)` during `AppBackend::initialize`.

**Source:** `src/backend/services/Logger.cpp`,
`include/backend/services/Logger.h`

## How to log

```cpp
#include <spdlog/spdlog.h>

SPDLOG_INFO("capture started: buffers={}", config.numBuffers);
SPDLOG_WARN("Modulus LUT not loaded - modulus gating will not be available");
SPDLOG_ERROR("failed to open {}: {}", path, err);
```

Prefer the `SPDLOG_<LEVEL>` macros over `spdlog::info(...)` because they
capture source file/line info.

## Log file location

`AppBackend::initialize` picks a user-writable location:

- Default: `<dataDir>/logs/app.log`.
- On Windows, if `dataDir` is under `Program Files`, fall back to
  `%LOCALAPPDATA%/MIB_Studio_Qt/logs/app.log`.

## Conventions

- **Never** use `std::cout` / `std::cerr` / `printf` / `qDebug`. Routing
  must be consistent for rotation and troubleshooting. See
  `knowledge_map/task/review_logging_improvements.md` for prior work.
- Every service's start and stop should log at INFO with a one-line
  summary (what it's doing, not what it "will do").
- Error logs should include enough context to triage without opening a
  debugger (path, errno-equivalent, device ID).

## Diagnostic logging

See `knowledge_map/task/diagnostic-logging.md` for the extended
diagnostic pattern used during camera lifecycle issues.

## Crash reporting

Process-level crash capture is owned by [[../services/CrashReporter]].
On unrecoverable failures (SEH violation, signal, uncaught C++
exception, Qt fatal) it writes a minidump + a JSON state snapshot
from [[../diagnostics/CrashStateMirror]] under
`%LOCALAPPDATA%/MIB_Studio_Qt/crashes/` and (when a Sentry DSN is
configured) submits them on next launch.

When you add a new service or a new long-running thread:
- Update its slot in `CrashStateMirror` at the same call sites where
  you already log lifecycle events (start/stop/error). See the
  "Wiring pattern" table in [[../diagnostics/CrashStateMirror]].
- Use `CrashReporter::breadcrumb("category", "message")` for one-shot
  events worth correlating with a crash but not worth a slot
  (e.g. "user opened HDF5 file X").
