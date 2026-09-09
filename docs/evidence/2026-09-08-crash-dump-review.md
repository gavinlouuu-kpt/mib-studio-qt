# Crash dump review — bench PC, 2026-09-08

Scope: every crash artifact the bench PC held from previous usage of MIB
Studio Qt, read on 2026-09-08 with WinDbg (`cdbX64`, Microsoft symbol
server, the dev build's PDB where it matched) and the app's own crash
sidecars (`CrashStateMirror` JSON next to each dump).

## Where the artifacts were

| Location | What | Count |
|---|---|---|
| `%LOCALAPPDATA%\CrashDumps\` (Windows Error Reporting) | full user dumps of `mib_studio_qt.exe` | 5 (2026-06-01, 06-03 ×3, 09-07) |
| same | frontend test binaries (`camera_action_state_test`, `ui_layout_test`) | 3 (2026-09-08, the offscreen-platform fail-fast fixed the same morning) |
| `%LOCALAPPDATA%\MIB_Studio_Qt\crashes\` (the app's crashpad queue) | `*-sigsegv.dmp` + `*.json` state sidecars, `*-exception.txt` | 62 reports / 55 dumps, 2026-08-05 → 2026-09-08, **none uploaded** (`.queued`/`.queued2`) |
| `C:\ProgramData\Microsoft\Windows\WER\ReportArchive\` | `AppCrash` (09-07) + two `AppHang` (09-03) for the installed app | 3 |

The app's crash directory is per user, not per build: the dev builds run
from `build\Release` write there too, so "no crash files under
`build\Release\data\crashes`" says nothing. Look in `%LOCALAPPDATA%`.

## 1. Installed app 1.0.7, 2026-09-07 15:52 — exit with an HDF5 file open

WER bucket `INVALID_POINTER_READ_c0000005_hdf5.dll!Unknown`, exception
`c0000005` at `hdf5!H5Pset_fapl_windows+0x4a0` (Conan hdf5 build
`69734edb`, identical bytes to the dev build's `hdf5.dll`). Symbolized
faulting stack (innermost first):

```
hdf5!H5FL_blk_free / H5FL_seq_free          <- freed chunk cache
hdf5!H5D__chunk_... / H5D_close
hdf5!H5VL__native_dataset_close / H5VL_dataset_close
hdf5!H5I_clear_type / H5D_top_term_package
hdf5!H5_term_library                         <- HDF5 atexit teardown
ucrtbase!execute_onexit_table
ntdll!LdrpCallInitRoutine  (hdf5.dll DLL_PROCESS_DETACH)
ntdll!LdrShutdownProcess / RtlExitUserProcess
kernel32!ExitProcessImplementation
ucrtbase!common_exit <- raise+0x11b
mib_studio_qt+0x1014de
```

Sidecar at the crash: capture running (5000 fps), realtime running,
`experiment_active=false`, **`hdf5.file_open=true`,
`hdf5.path=D:/data/guanshuo/2026.09.07/0.h5`**. The process was exiting
(this was the lingering stable instance being closed at the start of the
bench session) with that file still open; HDF5's own atexit handler then
closed the dataset whose chunk cache had already been released while a
writer was still active. 1.0.7 (link timestamp 2026-07-20) predates the
coordinator-owned finalization on PR #379.

Mitigation landed (both branches): `Hdf5Service` calls `H5dont_atexit()`
once before the first HDF5 call, so the library never tears itself down in
the CRT exit chain; files are closed by their owning services (recording
stop, experiment finalization, `Hdf5Service` destructor) and whatever is
still open at exit is leaked, never closed under a live writer. Guard:
`recording.hdf5_exit_teardown` (opens a file, appends on a detached writer,
`std::exit(0)` with the file open; must exit 0). Note: that test did **not**
reproduce the crash before the mitigation (6/6 clean), so it guards the
property, not the bug; the evidence for the mechanism is the symbolized
stack above.

## 2. Installed app 1.0.7 — 55 queued "sigsegv" reports (2026-08-05 → 09-08)

Every sidecar shows the app idle at the time of the report (capture not
running, no experiment, no file open), most with the last frame rate at
50 or 5000 fps, i.e. **crashes on exit after use**, silently caught by
crashpad. The same signature appears for the dev builds run on this bench
today (13:26, 14:14, 15:35 — each the close of a run that included an
experiment; a plain capture-only close did not trigger it). None of these
dumps could be symbolized: the app's own handler wrote the dump without the
exception pointers (exception record = the dump writer's breakpoint), and
the dev PDB no longer matched. **Fixed for future dumps** (same day): the
signal path now passes the CRT's `_pxcptinfoptrs` to `MiniDumpWriteDump`,
the sidecar carries a `"crash"` object (code, module+offset, thread,
access, `exe_build_id`), and `init()` keeps the running binary's PDB under
`%LOCALAPPDATA%\MIB_Studio_Qt\symbols\<build id>\`
(guard `backend.crash_reporter_segv`). Nine scripted start → record → stop → close
cycles with the current build (`27806d4c` + the accounting fix, then + the
reporter change) exited cleanly (exit 0, no new report); the crash is
intermittent, and its mechanism is most plausibly the same class as §1
(exit-time teardown of HDF5/thread state), which the mitigation also
covers. The next occurrence will be symbolizable: the dump now carries the
fault context and the sidecar names module+offset and the build id whose
PDB is kept in `%LOCALAPPDATA%\MIB_Studio_Qt\symbols\<build id>\`
(`cdbX64 -z <dmp> -y <that folder> -c "!analyze -v; ~*k"`).

**Why none of the 99 queued reports reached Sentry (2026-09-09).** Not the
network: from this PC a real 300 KB envelope posted with curl returned
HTTP 200, and a probe linking the same sentry-native 0.7.20 re-sent the
whole stuck queue through the same WinHTTP transport, 28/28 HTTP 200, no
rate limiting. The loss was lifecycle: the app only sends while it runs,
`sentry_close()` flushes for the library default of 2 s and dumps the
rest into the database `.run` dir, the backlog was 27 envelopes / 16.4 MB
and needed 19 s on this uplink, every bench session on 09-08 lasted
19–77 s, one ended in the exit-time segfault (the queue is lost outright
on a crash), the re-send of an old run is one-shot (files deleted before
sending), and the app's own `.queued` → `.queued2` policy declared 45
dumps terminal after one such attempt. Fixed the same day: pending dumps
are now posted directly to the minidump endpoint by `MinidumpUploader`
with a per-dump HTTP status, oldest first, ≤ 10 per launch, and the disk
queue advances only on a 2xx (`.sent`) or a permanent 4xx (`.rejected`);
legacy `.queued`/`.queued2` dumps re-enter the queue. Guard:
`backend.crash_reporter_pending_upload` (local fake endpoint: 200 / 400 /
503 / unreachable / per-launch cap). The probe already delivered the 27
stuck envelopes.

## 3. Installed app 1.0.7 — OpenCV ROI assertion (2026-08-18 ×2, 09-01 ×4, 09-07 ×1)

`*-exception.txt`: `OpenCV(4.12.0) matrix.cpp:807: (-215:Assertion failed)
0 <= roi.x && 0 <= roi.width && roi.x + roi.width <= m.cols && ... in
cv::Mat::Mat`. Sidecars: capture stopped, realtime stopped (two with
`realtime_running=true`), no experiment. An ROI larger than the frame is
applied to a `cv::Mat` somewhere on the non-experiment path (background or
ROI change against a frame of a different geometry). **Crash site identified and fixed** (same day): `ExperimentMonitoringTab`
cropped the accumulated monitoring frames (overlay and extract paths) with
the *current* ROI via an unclamped `cv::Rect(roi.x, roi.y, roi.w, roi.h)`;
frames accumulated under an earlier ROI / camera geometry (ROI edited,
camera or config switched, capture stopped — exactly the sidecar state)
trip the assertion. Both paths now go through
`frontend/tabs/MonitoringRoiCrop.h` (`cropToRoi`, clamped via
`clampRoiToFrame`); guard `frontend.monitoring_roi_crop`.

## 4. AppHang reports (2026-09-03), installed app

Two `AppHangB1` entries ("stopped responding and was closed"); no dump.
Consistent with the 108 s modal-during-finalization behaviour fixed on the
reliability branch (`931afe2c`, then moved into the coordinator).

## 5. Frontend test dumps (2026-09-08 morning)

`camera_action_state_test`, `ui_layout_test`: fail-fast `0xc0000409` from
the missing Qt offscreen platform plugin in headless CTest; fixed the same
morning by `cmake/MIBQtOffscreenTests.cmake`.
