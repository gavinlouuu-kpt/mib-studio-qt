# Entry page, Connect, Preview and Config tabs

Overview

- Connect tab: discover hardware cameras (Euresys) and configure Mock Camera. Connect only selects the camera; it does not start capture.
- Preview tab: shows live frames via `PlaybackPanel`. A centered Play/Stop overlay controls `CaptureService`.
- Config tabs: edit `include/config.json` and `include/egrabberConfig.js`. “Apply to Camera” runs the JS against the selected hardware device and keeps acquisition stopped.

Usage

1) Connect

- Open the Connect tab and press Refresh to enumerate devices.
- Pick a hardware camera and click Connect, or click “Configure Mock…” to browse a folder and FPS for the mock camera.
- After Connect, switch to Preview.

2) Preview and capture control

- Press Play to start capture (uses SDK pattern from 310-high-frame-rate.cpp).
- Press Stop to stop capture. Stats continue to update in the status bar.

3) Configuration

- App config (config.json): edit and Save.
- Camera script (egrabberConfig.js): edit, Save, and Apply to Camera. The backend stops capture if running, applies the script, issues `AcquisitionStop`, and remains stopped.
- Overview camera script (overviewConfig.js, the full-sensor script the Overview applies): edited on the Overview tab itself, under the live image (Reset, Save, Apply to Camera, Browse..., Clear). It is hidden for a MindVision camera. In the desktop (Tauri) app its default file is `overviewConfig.js` beside the active profile's `config.json`, else in the app config folder, and "Saved." / "Applied to camera. Capture remains stopped." appear as a line under the editor.

Notes

- Logging uses spdlog; no std::cout in app code.
- Discovery follows sample 150 (EGrabberDiscovery/eGenTL enumeration). Script execution uses `EGrabber::runScript`.
- CaptureService remains unchanged; we only swap its camera factory before Play.

