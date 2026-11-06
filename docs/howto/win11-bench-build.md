# Bench build on the Windows camera PC (win11-agent)

One command, run from P5, builds any branch of this repo on the Windows bench
PC with the real Euresys eGrabber SDK and prints the path of the runnable
`mib_studio_qt.exe`:

```bash
scripts/win11_bench_build.sh feat/my-camera-change
```

It exists because **PR CI never compiles the eGrabber code**: the Windows PR
check (`qt-ci.yml`) builds with the hardware SDKs off, and `build-windows.yml`
only runs on `develop` after the merge. A change to camera, capture or
eGrabber code is therefore built here and tested on the bench camera *before*
it merges (rule in [`AGENTS.md`](../../AGENTS.md#camera-capture-and-egrabber-changes)
and [`branching-and-releases.md`](branching-and-releases.md#feature-lane)). The
Windows beta is for handing already-tested builds to the rig, not for first
testing.

For the **Tauri** app, CI builds a camera-enabled portable candidate of any commit without this
PC: see [windows-tauri-camera-candidate.md](windows-tauri-camera-candidate.md).

## Build a branch

From any checkout of this repo on P5 (the build uses the commit, not the
working tree; uncommitted changes are not included):

```bash
scripts/win11_bench_build.sh <branch|tag|sha>   # build and wait
scripts/win11_bench_build.sh --clean <branch>   # also delete the CMake build tree first
scripts/win11_bench_build.sh --no-wait <branch> # start it and return
scripts/win11_bench_build.sh --attach [RUN_ID]  # wait for a running or finished build
scripts/win11_bench_build.sh --status           # one-shot status of the latest build
scripts/win11_bench_build.sh --log [RUN_ID]     # last 200 log lines
scripts/win11_bench_build.sh --all <branch>     # every target, tests included (default: mib_studio_qt only)
```

Exit status: 0 built, 1 build failed, 2 usage/ssh/push problem, 3 another build
is already running. A branch that exists only on GitHub needs a fetch first,
with credentials from Vault: `vault-agent-run github -- git fetch origin <branch>`.

What it does:

1. Resolves the ref to a commit and pushes it over ssh to a **bare repo on
   win11-agent** (`git push win11-agent:C:/MIBBuild/repo.git`). No GitHub
   credentials are ever placed on win11-agent.
2. Copies `scripts/win11_bench_build.ps1` to `C:\MIBBuild\bin` and runs its
   idempotent `Setup` step (a few seconds when nothing is missing).
3. Starts the build **detached** (a WMI-created process: a `Start-Process` child
   dies when the ssh session ends) and polls it every 30 s. A dropped ssh
   connection does not stop the build; use `--attach` to rejoin.
4. The Windows side checks the commit out into `C:\MIBBuild\src`, runs
   `conan install`, `cmake --preset windows-ninja-ci`, builds the target at
   below-normal priority (12 of 16 jobs, so the bench PC stays usable), checks
   the outputs, writes `build\Release\BENCH_BUILD.txt` (branch, sha, time) and
   runs a camera-safe start of the app (see [Running the app](#running-the-app)).
5. On success it prints `EXE: C:\MIBBuild\src\build\Release\mib_studio_qt.exe`;
   on failure the failing step, the log path
   (`C:\MIBBuild\logs\<run id>.log`) and the last 40 log lines.

Rebuilds are incremental: the Conan cache (`C:\MIBBuild\conan`), the checkout
and the Ninja tree persist, so touching one `.cpp` rebuilds in about a minute
(see the numbers in [the task note](../../knowledge_map/task/2026-11-04-win11-bench-build.md)).
Use `--clean` after a toolchain or dependency change.

One build runs at a time (`BUSY` and exit 3 otherwise). The exe is overwritten by
the next build, so copy it elsewhere if you need to keep it; `BENCH_BUILD.txt`
next to it says which commit it holds.

## What is on win11-agent

One-time setup, done on 2026-11-04 by `win11_bench_build.ps1 -Action Setup`
(re-run by every `win11_bench_build.sh`, so a rebuilt PC repairs itself). Nothing
is system-wide, no scheduled task or service exists, drivers and the existing
software are untouched.

| What | Version | Where | How it got there |
|---|---|---|---|
| Visual Studio 2022 Community | 17.14, MSVC 14.44 (`194`) | `C:\Program Files\Microsoft Visual Studio\2022\Community` | already installed |
| Euresys eGrabber SDK | headers at `C:\Program Files\Euresys\eGrabber\include` (CMake hard-codes this path) | `C:\Program Files\Euresys\eGrabber` | already installed |
| Git for Windows | 2.52.0 | `C:\Program Files\Git` | already installed |
| CMake | 4.2.1 | `C:\Program Files\CMake` | already installed |
| Python | 3.12.10 | `C:\MIBBuild\tools\python312` | python.org installer, **per-user** (`InstallAllUsers=0`, no PATH change, no launcher, no file associations); Authenticode signature checked (Python Software Foundation) |
| Conan | 2.34.0 | `C:\MIBBuild\tools\python312\Scripts\conan.exe` | `pip install "conan>=2.0,<3"` (`env/requirements-build.txt`) |
| Ninja | 1.13.2 | `C:\MIBBuild\tools\python312\Scripts\ninja.exe` | `pip install ninja` |
| numpy | 2.5.4 | same Python | `env/requirements-build.txt` |
| Conan home | n/a | `C:\MIBBuild\conan` (`CONAN_HOME`, set only inside the build) | created on first use; download retries added to `global.conf` |
| Conan remotes | n/a | `team-conan` = `https://conan.yofo.bio` (first), `conancenter` | **read-only, anonymous**; no login configured, nothing is ever uploaded from this PC |
| Bare repo | n/a | `C:\MIBBuild\repo.git` | `git init --bare`; P5 pushes `refs/heads/bench/<branch>` |

Layout under `C:\MIBBuild`: `repo.git`, `src` (checkout; `src\build` is the
Ninja tree), `conan`, `tools`, `logs` (`<run id>.log` and `<run id>.state.json`
per build), `bin` (the shipped scripts), `dl` (installer download).
`C:\Python310` on this PC is a stripped copy without a standard library and is
not usable; the Python above is separate. Ninja in `C:\Qt\Tools` is not used.

Disk: everything under `C:\MIBBuild` is about 3 GB (checkout and build tree 2.2 GB,
Conan cache 0.3 GB, bare repo 0.2 GB) because Qt, OpenCV and the rest arrive as
prebuilt packages; `Setup` refuses to run below 10 GB free.

### Why the team Conan remote

`conan install` needs the exact dependency graph CI uses. Against a **fresh**
Conan cache and conancenter alone it currently fails:

```
ERROR: Version conflict: Conflict between freetype/2.13.2 and freetype/2.15.0 in the graph.
Conflict originates from qt/6.7.3
```

`qt/6.7.3` and `opencv/4.12.0` pin `freetype/2.13.2`, while `ffmpeg` asks for
`freetype/[>=2.13.2 <3]`, which conancenter now resolves to 2.15.0. CI never sees
this because its Actions cache and the team remote already hold the old versions.
With `team-conan` first in the remote list the ranges resolve to CI's set (ffmpeg
7.1.3, freetype 2.13.2, hdf5, opencv 4.12.0, qt 6.7.3) and the prebuilt binaries
download in about a minute, instead of compiling Qt, OpenCV and ffmpeg. The remote
answers anonymous reads (`conan.yofo.bio/v2/ping`, search and downloads); if that
ever changes, do not copy credentials onto this PC: remove the remote
(`conan remote remove team-conan`), pin the conflicting versions in a lockfile or
`conanfile.py` override (`freetype/2.13.2`), and expect a multi-hour from-source
build.

## Deviations from the CI configure (`build-windows.yml`)

| CI | Bench build | Why |
|---|---|---|
| `-DMIB_ENABLE_MINDVISION=ON` with the pinned SDK provisioned from `updates.yofo.bio` (needs 7-Zip to unpack the installer) | `-DMIB_ENABLE_MINDVISION=OFF` | the rig has no MindVision camera or SDK, and the SDK would need another tool installed |
| `-DMIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI=ON` and the signer pin | not set | release-only signing gate; the option defaults to OFF |
| `-DMIB_RELEASE_VERSION_OVERRIDE`, `-DMIB_RELEASE_VERSION_FULL_OVERRIDE` | not set | version comes from the git tags (`cmake/MIBVersion.cmake`), `BENCH_BUILD.txt` carries the sha |
| Sentry DSN and environment | not set | local-only crash mode; benches must not report to the release Sentry project |
| `env/assets.json` required assets | skipped | none are marked required |
| eGrabber SDK installer, VC++ redistributable, InnoSetup installers | not run | the SDK is installed; packaging is a release job |
| sccache | not used | not installed; incremental Ninja is enough |
| Conan install without `-c` options | adds `-c tools.build:jobs=12` | leave CPU for the camera software; binary package ids are unaffected |
| `cmake --build build --config Release` (all targets, then `ctest`) | `--target mib_studio_qt` (or `--all`), no `ctest` | the bench job is "does the camera path compile and run"; unit tests run in CI |
| none | `-DMIB_ENABLE_HARDWARE_SDKS=ON` | explicit, so a stale CMake cache cannot turn the SDK off |

The script fails the build if CMake does not print `EGrabber SDK integration: ON`.
`MIB_ENABLE_MINDVISION=OFF` also needed a source fix on `develop`: see
[`CameraControlService`](../../knowledge_map/services/CameraControlService.md)
(`GenTLHolder.h` was only included inside the MindVision block, so the file did not
compile without MindVision; fixed in the same PR as this script).

## Running the app

The camera needs care. **Starting `mib_studio_qt.exe` with the Coaxlink camera
attached drives the camera by itself:** it picks the only camera, writes
`overviewConfig.js` to it (TTLIO11/TTLIO12 line modes, strobe, cycle timing,
exposure, window), and starts acquisition. Do not start it casually.

- The build's own start-up check sets `EURESYS_DEFAULT_GENTL_PRODUCER=grablink`
  for the child process only (documented in `EGenTL.h`; the SDK then loads
  `grablink.cti` instead of `coaxlink.cti`, so the app sees no camera), disables
  auto-update (`MIB_DISABLED_SERVICES=auto_update`), and kills the app at once if
  `app.log` ever mentions camera selection or capture. It proves the exe, Qt
  runtime and eGrabber library load and the app reaches its event loop. It does
  not exercise the camera; that is the bench test.
- For the real test, start the exe yourself from `C:\MIBBuild\src\build\Release`
  with `MIB_DISABLED_SERVICES=auto_update` (a beta-channel update must not replace
  a test build), make sure the camera is set to deliver frames (next section), and
  restore the camera when finished.
- The app is per-user: settings (`HKCU`, QSettings), `desktop.lock` and the LUT cache
  under `%LOCALAPPDATA%\MIB Studio` are shared with the installed beta in
  `C:\MIBTest\833`. Only one instance runs at a time; stop one before starting the
  other. Logs of the bench build are in `C:\MIBBuild\src\build\Release\data\logs`.

### GUI sessions and screenshots

A process started over ssh (or by the build's WMI launcher) lives in **session 0**:
its windows exist and answer messages but nothing is shown on the console, and an
ordinary screen grab shows nothing of them. Two things work:

- **UI Automation** (`System.Windows.Automation`) drives the real Qt UI: find
  tabs, buttons and text by name or AutomationId and invoke them. It is what the
  earlier bench session used (`C:\MIBTest\tools\lib2.ps1`: `ClickTab`,
  `ClickAsync`, `Dialogs`, `CapText`). Blocking calls such as "Start Experiment"
  open modal dialogs, so click them from a separate process (`clickasync.ps1`)
  and keep reading with the first.
- **Screenshots** need a different route than the screen: ask the window to paint
  itself (`PrintWindow` / `WM_PRINT` on the window handle). The same helpers file
  has the working variants. A closed-loop test can also read results from
  `data\logs\app.log`, which is usually clearer than a picture.
- Closing: `WM_CLOSE` to the main window ends the app when no experiment is
  running; otherwise a confirmation dialog waits invisibly in session 0, so stop
  the experiment first. `Stop-Process` is the fallback.

## The camera's bench state

The camera is an SVS-Vistek MC2066 on a Coaxlink Quad CXP-12 (interface
`KQQ07970`, `Device0`), run as a bench fixture. It is parked in this state, saved
as dumps in `C:\MIBTest\baseline\base_*.txt` (copied to `C:\MIBBuild\camera-baseline`
on first use):

| Where | Feature | Parked value |
|---|---|---|
| remote device | Width, Height | 1920, 1080 |
| remote device | OffsetX, OffsetY | 0, 0 |
| remote device | ExposureTime | 20 |
| device module | **CycleTriggerSource** | **DEL1_1** |
| device module | CycleMinimumPeriod, ExposureRecoveryTime | 40000, 1000 |
| device module | StrobeDelay, StrobeDuration | 0, 1000 |
| interface module | TTLIO11, TTLIO12 | LineMode Input, LineSource Low, TTLIO11 not inverted |

`CycleTriggerSource = DEL1_1` means the camera cycle is clocked by a delay line
that is not running, so **no frames arrive**. To get frames for a test set it to
`Immediate` first (`gentl.exe genapi --module=dev --set CycleTriggerSource Immediate`,
with `gentl.exe` from `C:\Program Files\Euresys\eGrabber\bin\x86_64`). A MIB Studio
run then changes ExposureTime, window, cycle timing, strobe and the TTLIO lines
(see `resources/defaults/overviewConfig.js`), and killing the app can leave the
camera in `AcquisitionStart` (`TLParamsLocked = 1`).

**Restore** after every camera test, from P5:

```bash
scp scripts/win11_bench_camera.ps1 win11-agent:C:/MIBBuild/bin/
ssh win11-agent 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\MIBBuild\bin\win11_bench_camera.ps1 -Action Check'    # read-only
ssh win11-agent 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\MIBBuild\bin\win11_bench_camera.ps1 -Action Restore'  # write back, then Check
```

`Check` dumps the four GenICam modules read-only and diffs them with the baseline,
ignoring counters (`RESULT=identical`, `different`, or `incomplete` when a module
could not be read because a program has the data stream open). `Restore` runs
`AcquisitionStop`, writes back Width 1920, Height 1080, OffsetX 0, OffsetY 0,
ExposureTime 20, the device-module timing and `CycleTriggerSource DEL1_1`, and the
TTLIO11/12 lines, then runs `Check`. `Restore` **refuses to run while any
`mib_studio_qt.exe` is running** (`-Force` overrides): somebody's test is using the
camera and would be overwritten. Both actions talk to the camera; the build script
itself never calls them, and neither should run while another session is testing.

## Failure modes seen so far

- `conan install` version conflict on `freetype`: the team remote is missing or
  not first (`conan remote list` with `CONAN_HOME=C:\MIBBuild\conan`). See above.
- `C3083`/`C2039` on `backend::camera::egrabber::sharedGenTL` in
  `CameraControlService.cpp`: the branch predates the include fix. Rebase on
  `develop`.
- `BUSY`: another build is running; `--attach` it.
- ssh `BatchMode` refused: the key for `win11-agent` is not loaded; `ssh win11-agent
  hostname` tells.
