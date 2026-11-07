# Qt vs React/Tauri parity check (2026-11-04)

Status: complete. Commit tested: origin/develop `5f906ac2`. Mock camera only; no hardware.

> **Snapshot.** This describes develop `5f906ac2` (2026-11-04). File:line references apply to that commit (#871 has since split `desktop/src/App.tsx`), and the open Tauri parity PRs of program #869 (#870, #872–#898) may close gaps listed here.

Comparison page (screenshots side by side): https://claude.ai/artifact/AqMayU6NpF7d9TK51fzLQm
Feature matrix: [2026-11-04-tauri-parity-matrix.md](2026-11-04-tauri-parity-matrix.md)
Raw outputs on P5: `/mnt/hdd/developer-data/mib-studio-qt/parity-20261104/` (`qt/`, `tauri/`, `inventory/`)

## Verdict

The Tauri app is not ready to replace Qt yet. It is stable on the mock path but well short of feature parity.

- **Stable:** all of these pass: 764 frontend tests, 147 Rust tests (mib-bridge 42, mib-app-commands 31, mib-bridge-server 59, src-tauri 15) and `native-workflow.py` against the release binary. The browser tour (Playwright, about 66 captures) logged no console errors and no uncaught exceptions. Connect → preview → run → finalize → Review → export → regenerate masks works natively and in the browser. A page reload mid-run recovers in about 100 ms, and six camera start/stop cycles were clean.
- **Not at parity:** of the matrix's 589 rows, 499 are Qt behaviours: 104 match, 283 are partial and 112 are missing. The other 90 are Tauri-only. By severity: 1 blocker, 88 major, 306 minor. Most Qt work since 2026-10-01 is not ported: #831, #833/#834, #705, #576, #492, #472, #589. At the snapshot, the last desktop change on develop was on 2026-10-22.
- **Release gates still open:** packaging and update feed, real-hardware acceptance, and performance under real load (`docs/exec-plans/active/2026-09-23-tauri-replacement.md`).

## Must close before migrating

1. Blocker: Export FCS and Batch Export FCS (#576) are missing from the Tauri bridge.
2. Monitoring live tuning: tune panel, Young's modulus gate and draggable gate (#831), click-to-view (#705), cell strips with rejection reasons, Monitoring Settings.
3. Recording-capacity panel and its Start gates (#833/#834); profile `camera.window`.
4. Latest Frame acknowledgement and the Every/Latest Frame badge: nothing sets `acknowledgeLatestFrameDrops`.
5. Overview: ROI drag with script write-back, camera script editor and auto-apply, Wafer Grid (#472).
6. Config: Save writes only `image_processing`; the profile camera script is never applied; no core-line selector (#492); no file watcher.
7. Hardware: pump and autofocus only on Connect, Service-mode gated, locked during a run; the pump is not polled.
8. Shell: no Diagnostics in the native app, no alert banner, no single-instance guard, no raw-recording fault latch (#589).

## Bugs found in the Tauri version

- Major: Review layout regression. The Regenerate masks panel fills Valid/Invalid/Charts; `App.css:958` stopped matching after the ViewerGate wrapper at `App.tsx:2428` (#550 c2).
- Major: Review exports pass the live px→µm factor (`App.tsx:2370-2377`), not the file's recorded factor (Qt: `HdfReviewTab.cpp:1296,1341,1844`). Found by code reading; not yet reproduced with a file.
- Major: pump state is never polled (`HardwareControls.tsx:70-95`). Stop pump and Disable autofocus are disabled while another hardware command is in flight.
- Minor: the large frame viewer does not enlarge (#550 m7); sidebar telemetry (#550 m2/m3/m8); stale banners (#550 m6); Help › User Manual is the Qt manual; `experimentCompleted` is hard-coded false (`App.tsx:1229`); the preflight storage check is a stub; camera JSON editors fail in browser mode; HDF5 "open objects before close" on each Review close (shared backend).

## Method

- Qt: Release `mib_studio_qt` built from 5f906ac2, driven with xdotool under Xvfb at 1600×1000.
- Tauri: the same commit. The browser UI was driven by Playwright against `yofo-studio-server` on 127.0.0.1. The native release binary was driven through tauri-driver.
- Shared inputs: the mock 512×96 1000-frame set and `review-parity/inputs/population_3000.h5`.
- The feature matrix comes from source reading, with file:line on both sides and spot checks by hand.
- The Qt mock run processed 0 cells because the saved ROI sat outside the mock frame. That is a test-setup artifact.
