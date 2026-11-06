## 2026-11-07 — Camera-enabled Windows Tauri candidate from CI (#869)

Every Tauri rig check needs a Tauri build that can open the camera. win11-agent, where those were
built by hand, is down. The Windows Tauri candidate workflow therefore takes two dispatch inputs:
`ref` (the commit to build) and `camera_sdk`. With `camera_sdk=true` the job installs the eGrabber
SDK 25.10.0.57 as `build-windows.yml` does, and also pins its SHA-256. It builds the backend with
`MIB_ENABLE_HARDWARE_SDKS=ON` and MindVision off, and uploads
`unsigned-egrabber-windows-tauri-<built sha>`.

The package ships no eGrabber DLL or GenTL producer. The exe uses the eGrabber install on the PC,
as the Qt app does. The package ships no Windows OS DLL either. Camera mode applies #870's #851
rule even when the built ref lacks #870: develop's packaging staged about 30 of the runner's OS
DLLs (kernel32, d3d11, mf*, …), and camera mode deletes them before the smoke run. The workflow proves the backend is linked: the link manifest defines
`MIB_HAS_EGRABBER=1`, the exe contains backend strings that only such a build has, and
`dumpbin /dependents` runs. The SDK-free default and the push trigger are unchanged. The first
commit is #870's Conan fix (`102fbec7`), cherry-picked.

`desktop/scripts/rig-tauri-gui-probe.ps1` is a paste-ready probe for the rig session. It tries CDP
over WebView2's DevTools port (127.0.0.1), then UI Automation, and it has not been run on the rig
yet. How-to: `docs/howto/windows-tauri-camera-candidate.md`. See
[[architecture/Desktop-Shell]] ("Windows nonpublishing candidate lane").
