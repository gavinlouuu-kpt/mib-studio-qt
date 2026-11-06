# Camera-enabled Windows Tauri candidate from CI

The **Windows Tauri candidate** workflow (`.github/workflows/desktop-windows-candidate.yml`) builds an
unsigned, portable Windows x64 package of the Tauri desktop app for any commit. By default it is
SDK-free, so its exe cannot open a camera. With `camera_sdk=true` it is built with the Euresys
eGrabber SDK, so it can drive the Coaxlink camera on the rig, as MIB Studio (Qt) does. Use this
build for Tauri rig checks of camera, capture and Connect changes. It needs no Windows build PC:
the [bench build](win11-bench-build.md) on win11-agent is the other route, and it builds the Qt app.

## Dispatch a camera build

```bash
# Full 40-character SHA of the commit to build (a branch or tag name also works; a short SHA does not).
sha=$(vault-agent-run github -- gh pr view <PR> --json headRefOid -q .headRefOid)
vault-agent-run github -- gh workflow run desktop-windows-candidate.yml \
  --ref develop -f ref=$sha -f camera_sdk=true
```

- `--ref` picks the version of the workflow file that runs. Use `develop` once the camera mode is on
  develop; before that, use the branch that adds it (`ci/tauri-camera-candidate`). Every script,
  tool and source file comes from `ref`, the commit being built.
- `ref` empty: the commit of `--ref`. `camera_sdk` omitted: the SDK-free candidate, unchanged.
- A run takes about 20 minutes. Dispatches for different commits queue separately.

Find the run: its title is `Windows Tauri candidate (eGrabber) <ref>`.

```bash
vault-agent-run github -- gh run list --workflow desktop-windows-candidate.yml --limit 5
```

## Where the artifact lands

- Artifact `unsigned-egrabber-windows-tauri-<built sha>` on the run (SDK-free:
  `unsigned-sdk-free-windows-tauri-<built sha>`). It holds `tauri-windows-candidate.zip`.
- The run's job summary lists the built commit, the artifact name, and the sha256 of
  `mib-studio-desktop.exe` and of the zip.
- Download it: `vault-agent-run github -- gh run download <run id> -n <artifact name>`. Artifacts
  expire with the repository's artifact retention.

The zip holds `mib-studio-desktop.exe` and its DLL closure. The closure is the Conan DLLs, the
CoreMOR `XMT_DLL_SER.dll` and the app-local MSVC runtime. The zip also holds `resources/` (defaults,
isoelastic LUT), a `README.txt` that names the commit, the run and the backend features, and
`SHA256SUMS.txt`. There is also `os-dlls-removed.txt` when the camera check had to drop OS DLLs.
Extract the whole zip into one folder.

## What the rig PC needs

- **The eGrabber install that MIB Studio (Qt) uses:** Euresys eGrabber 25.10.0.57 in
  `C:\Program Files\Euresys\eGrabber`, with its Coaxlink driver and GenTL producer. The Qt installer
  installs it when it is missing (task "Install eGrabber SDK"), so a rig PC that runs MIB Studio
  already has it.
  - The candidate does **not** ship the eGrabber DLLs or the GenTL producer, and the workflow fails
    if any of them end up in the package.
  - eGrabber is header-only in our build, and opens the producer when the app runs. It finds the
    producer through the system environment variables that the eGrabber installer sets. A
    25.10.0.57 install on the runner set (run 42250967806):
    - `EURESYS_DEFAULT_GENTL_PRODUCER=coaxlink`;
    - `EURESYS_COAXLINK_GENTL64_CTI=C:\Program Files\Euresys\eGrabber\cti\x86_64\coaxlink.cti`;
    - `EURESYS_EGRABBER_LIB64` and `GENICAM_GENTL64_PATH`, both `C:\Program Files\Euresys\eGrabber\cti\x86_64`;
    - the grablink, gigelink and playlink `*_GENTL64_CTI` variables.
  - A rig PC can check for the install with
    `[Environment]::GetEnvironmentVariable('EURESYS_COAXLINK_GENTL64_CTI','Machine')` and
    `Test-Path 'C:\Program Files\Euresys\eGrabber\cti\x86_64\coaxlink.cti'`.
  - Without the install, the app still starts but finds no eGrabber camera.
- **Microsoft Edge WebView2 Evergreen Runtime** (present on current Windows 11).
- No other program holding the camera: close MIB Studio (Qt), eGrabber Studio and any `gentl.exe`
  session first.

The candidate is not installed. It writes its state under `%LOCALAPPDATA%`, has no updater, and is
not signed, so SmartScreen may ask once.

## What the workflow checks in camera mode

1. The installer has the pinned size and SHA-256, the silent install exits 0, and the headers exist.
2. CMake reports `EGrabber SDK integration: ON`, and `build/mib-bridge-link-manifest.json` defines
   `MIB_HAS_EGRABBER=1`, so the Rust bridge compiles against the eGrabber backend.
3. The usual frontend, bridge and desktop tests, then the DLL closure, then a 10 s smoke run of the
   staged exe with `PATH` reduced to System32 and `MIB_CAMERA_MODE=mock`. The runner has no camera.
4. No eGrabber, GenTL or Euresys file is packaged: no `.cti`, and no DLL that the eGrabber install
   also has.
5. No Windows OS DLL is shipped (#851). This is #870's rule. A built ref without #870's packaging fix
   stages the runner's copies (kernel32, d3d11, dxgi, mf*, …), and a rig PC on another Windows build
   would load them from the app folder. Camera mode deletes every staged DLL that System32 also has,
   except the app-local MSVC runtime, and lists them in `os-dlls-removed.txt`. The smoke run then
   starts the cleaned package.
6. The exe contains `AppBackend: configuring hardware EGrabber camera`. That code exists only in an
   `MIB_HAS_EGRABBER=1` build. The exe does not contain the SDK-free fallback
   `EGrabber SDK is unavailable in this build`. `dumpbin /dependents` shows no eGrabber import.

Backend flags in camera mode: `MIB_ENABLE_HARDWARE_SDKS=ON` and CoreMOR at its Windows default (on),
as in the Qt Windows build, plus the processing-core signer pin from the same repository variable.
`MIB_ENABLE_MINDVISION=OFF`, `MIB_USE_SENTRY=OFF` and `MIB_ENABLE_WINDOWS_PACKAGING=OFF`: the
candidate has no MindVision SDK, no Sentry DSN and no installer.

## Driving the Tauri GUI on the rig

The rig session drives apps from PowerShell. Before any Tauri rig steps, run
`desktop/scripts/rig-tauri-gui-probe.ps1` (at most about 2 minutes). It starts the candidate with the
mock camera and WebView2's DevTools port (`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333`).
The port listens on 127.0.0.1 only. Over the Chrome DevTools Protocol, the probe reads
`document.title` and opens and closes the Help menu. If that fails, it looks for the Help button with
UI Automation. Then it closes the app.

The rig PC cannot fetch from P5. Paste the script into a file there, for example with
`Set-Content C:\MIBTest\rig-tauri-gui-probe.ps1 -Encoding ASCII -Value @' ... '@`. Then run it:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\MIBTest\rig-tauri-gui-probe.ps1 -Exe C:\MIBTest\tauri-<sha>\mib-studio-desktop.exe
```

The last line of its output is the result:
- `RESULT: CDP`: drive the GUI over CDP.
- `RESULT: UIA`: drive it with UI Automation.
- `RESULT: no Tauri GUI control` (exit code 2): stop and report.
