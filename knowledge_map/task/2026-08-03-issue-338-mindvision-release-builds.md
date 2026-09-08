# 2026-08-03 — Default-on MindVision builds on every desktop OS (#338)

## Problem

Official beta and stable installers explicitly configured
`MIB_ENABLE_MINDVISION=OFF`, so `MIB_HAS_MINDVISION=0` compiled camera
discovery and capture down to stubs. A connected MindVision camera therefore
could not be found by any CI-produced v1.0.6/v1.0.7 installer. Follow-up R2
inspection found maintained vendor packages for Linux and macOS as well, so
the fix was widened from the Windows publisher to all desktop builds.

## SDK sources and trust pins

All three platform archives are mirrored under the team R2
`mindvision-sdk/` prefix and are checksum-pinned by the provisioners:

| OS | R2 object | Size | SHA-256 | Selected inputs |
|---|---|---:|---|---|
| Windows x64 | `MindVision-Camera-Platform-Setup2.1.10.195_202604021438.exe` | 190,201,869 | `a62f58a8aef103d0061dc2c12b709d0655136cc488840e22361dff08adc4d4f4` | `Demo/VC++/Include/*`, `SDK/X64/MVCAMSDK_X64.dll` |
| Linux | `linuxSDK_V2.1.0.49202602041120.tar.gz` | 63,075,119 | `246d374dc7f91a8fa7120ceced020680a2b249bbfcf2d974d4e0ed0c04cc6313` | `include/*`, architecture-matched `libMVSDK.so`, udev rules |
| macOS | `mac-sdk.rar` | 10,833,965 | `ae3358bcb24a10275248ef5e7cc0c5507dbe804436112528b38c266eed140014` | arm64 or x86_64 nested SDK: `include/*`, `libmvsdk.dylib` |

`scripts/provision-mindvision-sdk.ps1` handles Windows;
`scripts/provision-mindvision-sdk.sh` selects Linux or macOS plus the current
CPU architecture. Both fail closed on checksum mismatch, missing
headers/runtime, or absence of `CameraGetImageBufferPriority`. The macOS
provisioner changes the vendor app-bundle-only install name to `@rpath` and
re-applies an ad-hoc signature so normal CMake build trees can load it.

## Resolution

- `MIB_ENABLE_MINDVISION` defaults to `ON` on Windows, Linux, and macOS; all
  maintained desktop presets also pin it on explicitly.
- Processing-only builds do not activate camera SDK discovery and remain
  portable/SDK-free.
- Windows uses `CameraApiLoad.h` and runtime DLL loading. Linux/macOS include
  `CameraApi.h` and link `libMVSDK.so` / `libmvsdk.dylib` directly.
- Both GitHub desktop release workflows and local `release.ps1` provision the
  pinned SDK, explicitly configure MindVision on, and verify that
  `MVCAMSDK_X64.dll` reaches the Release payload.
- The verified SDK enables `MIB_MINDVISION_USE_PRIORITY_API=1`, so Latest Frame
  mode uses the native newest-buffer API instead of the bounded-drain fallback.
- `scripts/test_mindvision_release_gate.py` prevents any release entry point
  from reverting to `MIB_ENABLE_MINDVISION=OFF` or dropping SDK provisioning.
- Linux backend, sanitizer, soak, and native-processing-core CI jobs provision
  the pinned Linux SDK before their default-on configure.

## Deployment boundary

Windows installers include `MVCAMSDK_X64.dll` next to the app. They do not
embed or run the 181 MiB vendor installer; a target machine still needs the
MindVision device driver installed for hardware enumeration. Linux build trees
link the selected architecture's `.so`; Linux operators must install the
provided udev rules. A future macOS app bundle must copy `libmvsdk.dylib` into
its runtime payload.

## Verification

- Regression-first gate failed against the old release configuration and
  passes after the change.
- All three R2 URLs returned HTTP 200; downloaded bytes matched the pins.
- Windows/Linux/macOS header inspection confirmed `CameraGetImageBufferPriority`,
  `CAMERA_GET_IMAGE_PRIORITY_NEWEST`, software/external-trigger, delay/count,
  jitter, and strobe symbols.
- Linux headers compiled in an x86_64 cross-target probe. On Apple Silicon,
  the default-on backend configured and compiled `MindVisionCamera`,
  `MindVisionApply`, and `CameraControlService` against the arm64 dylib; RPATH,
  SDK initialization, and code-signature loading succeeded. Seven focused
  camera/release CTests passed.
- The full Linux backend build and Windows installer build remain CI/platform
  verification. An attempted all-target macOS build reached an unrelated
  pre-existing OpenCV 5 test compile error (`cv::boundingRect` include) after
  the backend and focused camera targets had already built successfully.

## Windows beta follow-up

The first beta run from the merged change successfully downloaded and
validated the Windows SDK, configured with MindVision enabled, and compiled
the complete app. CTest then exposed two test-only portability assumptions:
Windows does not surface Git's Unix executable bit through `stat`, and its
coarser sleep scheduling did not reliably overload the simulated camera with a
5 ms consumer pause. The release gate now checks `S_IXUSR` only on POSIX, and
the overload workload uses a 50 ms pause while continuing to gate on logical
queue/sequence relationships and cross-mode age ratios rather than absolute
timings. Its freshness comparison uses the newest frame that actually reached
the completed-buffer queue; capture attempts rejected by a full queue remain
accounted underruns rather than impossible delivery candidates. Production
camera and release behavior are unchanged.
