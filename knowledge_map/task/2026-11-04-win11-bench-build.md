# 2026-11-04 — Build any branch on the Windows camera PC (win11-agent)

## Goal

Camera, capture and eGrabber PRs are tested on the real camera before they merge,
but PR CI never compiles `MIB_HAS_EGRABBER` code (`qt-ci.yml` builds with the
hardware SDKs off; `build-windows.yml` runs after the merge). `scripts/win11_bench_build.sh
<branch>` puts a branch on the Windows bench PC and builds it there with the real
eGrabber SDK. How to use it: `docs/howto/win11-bench-build.md`; the rule is in
`AGENTS.md` and `docs/howto/branching-and-releases.md`; overview in
[[build-and-run/Build]].

## What was found on the way

| Finding | Consequence |
|---|---|
| `conan install` of `develop` against an empty Conan cache and conancenter alone fails: `Version conflict: Conflict between freetype/2.13.2 and freetype/2.15.0` (qt and opencv pin 2.13.2, ffmpeg asks `[>=2.13.2 <3]`) | CI only works because its caches already hold the old versions. The team remote `https://conan.yofo.bio` answers anonymous reads and serves CI's exact binaries; with it first in the remote list the graph resolves like CI's and Qt/OpenCV download prebuilt (23 s). No credentials are used or copied. |
| `develop` does not compile with `-DMIB_ENABLE_MINDVISION=OFF`: `CameraControlService.cpp` included `GenTLHolder.h` only inside the MindVision block, but calls `sharedGenTL()` under `MIB_HAS_EGRABBER` (C3083/C2039) | fixed in this PR (`fix(camera): include GenTLHolder.h whenever the eGrabber SDK is on`); the first bench build is the first compile of that combination. The bench has no MindVision SDK, so it builds with MindVision off. |
| Starting `mib_studio_qt.exe` with the Coaxlink camera attached selects the camera by itself, applies `overviewConfig.js` to it (TTLIO11/12 line modes, strobe, cycle timing, exposure, window), configures the trigger output and starts acquisition, within about 8 s | The first start-up check did exactly that for about a second, then the process was killed, leaving the camera changed (and `AcquisitionStart` sent). The check now masks the Coaxlink producer (`EURESYS_DEFAULT_GENTL_PRODUCER=grablink`, child process only), disables auto-update and kills the app if `app.log` mentions camera selection. `scripts/win11_bench_camera.ps1` checks and restores the parked camera state. |

## Numbers (win11-agent: Ryzen 7 5700G, 8 cores/16 threads, 28 GB; build at below-normal priority, 12 jobs)

| Run | Wall time | Notes |
|---|---|---|
| Cold: empty Conan cache, clean tree (`--clean`) | 4 m 06 s | conan install 23 s (prebuilt packages), configure 38 s (includes cloning sentry-native), build of `mib_studio_qt` 180 s (445 Ninja steps, windeployqt included) |
| Clean tree, warm Conan cache | 3 m 36 s | configure 37 s, build 149 s, smoke start 20 s |
| Incremental: touch `CameraControlService.cpp` | 38 s | conan 4 s, configure 2 s, build 14 s |
| One-time tool setup | about 1 m | Python 3.12.10 installer and pip packages |

Disk: `C:\MIBBuild` is about 3 GB. A build of `develop` as of 5f906ac with the include fix is
the benchmark; larger PRs rebuild more.

## Not done / not possible here

- `develop` itself (5f906ac) does not build in the bench configuration (the include bug above);
  the first full build is `develop` plus that one commit.
- MindVision-on bench builds (as CI) would need the pinned SDK installer and 7-Zip on win11-agent.
- sccache, `ctest` and the screenshot tour are not run on the bench.
