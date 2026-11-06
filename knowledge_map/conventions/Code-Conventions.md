# Code Conventions

> Distilled from top-level `AGENTS.md` and recurring review feedback.

## Logging

- Use **spdlog**; never `std::cout` or `printf` in app code.
- See [[Logging]] for log paths and levels.

## Source / header layout

- Headers live under `include/` and **mirror** the `src/` tree
  (`src/backend/services/Foo.cpp` ↔ `include/backend/services/Foo.h`).
- Exception: `include/Coremor/` is a third-party SDK (nanopositioner DLL)
  — do not reorganize.

## Reuse

- Review `src/backend/Tools.cpp` (`include/backend/Tools.h`) before
  writing new utility helpers: it already has `getTimestamp`,
  `availableComPortNumbers`, `getProcessMemoryMB`,
  `getAvailableSystemRAMBytes`, etc.
- Before implementing a new camera feature, **check `egrabber-sample-programs/`**
  for a ready-made pattern. See [[../camera/EGrabberCamera]].

## Runtime data

- Runtime state lives in the data dir: `<exe dir>/data` in dev builds and on
  Linux, `%LOCALAPPDATA%/MIB_Studio_Qt/data` in a Windows install under Program
  Files (logs then in `%LOCALAPPDATA%/MIB_Studio_Qt/logs/`), or `MIB_DATA_DIR`.
  Resolve paths from `AppBackend::dataDir()`, never from the exe dir; a new state
  file is added to `datadir::stateEntries()` (see "Data directory" in
  [[../architecture/AppBackend]]).
- Do not check experiment HDF5 files into git.

## Task / docs housekeeping

- Operational tasks + in-flight work go in `knowledge_map/task/` (dated
  filename convention `YYYY-MM-DD-<slug>.md` is encouraged but not
  enforced).
- User-facing guides go in `docs/howto/` and `docs/integration/`.
- Agent onboarding lives in this vault (`knowledge_map/`).

## Camera capture

- Refresh EGrabber `StreamModule` counters **before** calling
  `CaptureService::stop()`; otherwise the final FPS/MBs readout is zero.
  See task `fps_mbs_zero.md` and `docs/howto/safe-start-stop-egrabber.md`.

## Thread safety

- Never touch a service's private state outside its public API.
- UI ↔ non-Qt-thread communication goes through
  `backend::BackgroundCaptureNotifier` or Qt queued connections. See
  [[../frontend/System-Utilities]].

## C++ standard

- `CMakeLists.txt` sets C++17. Prefer `std::filesystem`, `std::optional`,
  `std::string_view` where natural.
