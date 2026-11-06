# 2026-11-07 — Camera-enabled Windows Tauri candidate from CI (#869, PR #911)

## Goal

Every Tauri rig check (#877, #891, the Connect PRs, #881's HW-057, #870's re-test) needs a Tauri
build that can open the Coaxlink camera. win11-agent, where those were built by hand, is down. The
Windows Tauri candidate workflow now builds any commit with the eGrabber SDK when it is dispatched
with `camera_sdk=true`. How to use it: `docs/howto/windows-tauri-camera-candidate.md`. The lane is
described in [[architecture/Desktop-Shell]] ("Windows nonpublishing candidate lane").

## Steps

1. Cherry-picked #870's Conan fix (`102fbec7`, team remote first). Without it, the lane fails in
   Conan install on develop: run 42214710507, ffmpeg source build, "aom >= 2.0.0 not found".
2. Added the `ref` and `camera_sdk` dispatch inputs. Camera mode:
   - installs eGrabber SDK 25.10.0.57 as `build-windows.yml` does, and also pins its SHA-256;
   - configures `MIB_ENABLE_HARDWARE_SDKS=ON` and MindVision off, with CoreMOR at its default (on);
   - checks the link manifest for `MIB_HAS_EGRABBER=1`;
   - checks the exe for code that only an eGrabber build contains, and logs `dumpbin /dependents`.
   The camera checks run inline in the workflow, because the packaging script comes from the
   built ref.
3. Wrote a rig GUI probe (`desktop/scripts/rig-tauri-gui-probe.ps1`). It tries CDP on WebView2's
   DevTools port, then UI Automation, and has not been run on the rig.

## What was found on the way

| Finding | Consequence |
|---|---|
| eGrabber is header-only in our build: `EGenTL` opens `coaxlink.cti` when the app runs, through the installer's system variables (`EURESYS_COAXLINK_GENTL64_CTI`, `GENICAM_GENTL64_PATH`, ...). | The exe imports no eGrabber DLL, so `dumpbin` cannot prove the link. The proof is the manifest define plus backend strings on both sides of `#if MIB_HAS_EGRABBER`. The rig PC needs the eGrabber install that Qt uses. |
| A name pattern `^EGrabber` matched `resources/defaults/egrabberConfig.js` (run 42250967806). | The no-SDK-files check now compares against the DLL names of the eGrabber install (88 on 25.10.0.57), plus any `.cti`. |
| develop's packaging stages about 30 of the runner's OS DLLs (kernel32, d3d11, dxgi, mf*, ...): #851, fixed on #870. Found by downloading the artifact of run 42253873880, a green run. | Camera mode deletes each staged DLL whose recorded source (`native-runtime-files.txt`) is under `%SystemRoot%`, before the smoke run. Artifacts of runs 42253873880 and 42253976511 are superseded. |
| GitHub no longer accepts `runs-on: macos-14` ("The configuration 'macos-14-arm64-us-default' is not supported"). | The macOS DMG lane of YOFO Review CI fails on every PR (#903). Not part of this change. |
| Review of #911 (approve, no blocking findings), five things to fix:<br>- the probe clicked before React rendered the menu bar;<br>- the probe did not check that the page was this app's;<br>- the UIA lookup was not bounded;<br>- the OS-DLL removal went by name;<br>- one step interpolated `github.ref` into PowerShell. | The probe now waits for the Help button, takes only a `tauri.localhost` page, refuses a busy port, and runs UIA in a runspace waited for 30 s. The DLL removal goes by recorded source path. The step reads the refs from `env:`. |

## Open

- The rig GUI probe and the candidate itself are untested on the rig PC. mib-pm queues them.
- Use `--ref develop` for dispatches once #911 merges.
