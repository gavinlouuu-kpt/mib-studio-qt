## 2026-11-04 — Bench build on the Windows camera PC; camera/eGrabber PRs are bench-tested before merge

`scripts/win11_bench_build.sh <branch>` (run from P5) builds any branch on `win11-agent` with
the real Euresys eGrabber SDK and prints the path of `mib_studio_qt.exe`: the commit is pushed
over ssh to a bare repo there (no GitHub credentials leave P5), `scripts/win11_bench_build.ps1`
runs detached (Conan from the anonymous team remote, `windows-ninja-ci` preset, MindVision
off, Release build, a camera-safe start of the exe). Cold build 4 m 06 s, incremental 38 s.
New rule in `AGENTS.md` and `docs/howto/branching-and-releases.md`: a PR that touches camera,
capture or eGrabber code is built this way and tested on the bench camera before it merges,
because PR CI does not compile the eGrabber code. The first bench build found that `develop`
did not compile without MindVision (`CameraControlService.cpp` only included `GenTLHolder.h`
inside the MindVision block); fixed here. `scripts/win11_bench_camera.ps1` checks and restores
the bench camera's parked state after a test (starting the app drives the camera and changes
it). Guide: `docs/howto/win11-bench-build.md`; details: [[task/2026-11-04-win11-bench-build]],
[[build-and-run/Build]], [[camera/EGrabberCamera]], [[services/CameraControlService]].
- **Camera check fix (from the #842 bench test):** every `gentl genapi --ro --module=remote --dump` ends with `Ignoring error GC_ERR_ABORT`, the baseline too, so `win11_bench_camera.ps1 -Action Check` always skipped the remote module and never reported `identical`. Only a `GC_ERR_*` line that does not start with "Ignoring error" now marks a module unreadable, and that line is excluded from the diff.
