## 2026-11-04 — eGrabber camera window, stage scripts and recording capacity in the desktop (#863, #846)

The Tauri Overview/Experiment stages now drive an eGrabber camera as the Qt app does. Entering Overview
applies the Overview script (full sensor), entering Experiment the Experiment script followed by the
profile's `camera.window`, with the camera stopped and restarted around it; Overview starts a stopped,
configured camera. The window box follows the profile's window (else the Experiment script's, read from
the applied profile's `egrabberConfig.js` before any script ran), snaps the way Qt does and is saved into
the Experiment script and the applied profile, whose remembered revision follows so a restart restores
the edited window (also when only the profile's script changed). The Overview shows the backend's
Recording capacity, and a geometry the backend cannot give is a line in the Overview instead of a silent
null. Bridge ABI 45: `apply_camera_script_with_role`, `save_camera_window`, `fetch_recording_capacity`,
additive geometry members. Bug #846 (the early return on `supported=false`) is closed by an eGrabber
geometry. The Overview's own script editor (`overviewConfig.js`) is not part of this change. See
[[architecture/Rust-Bridge]], [[architecture/AppBackend]] and [[architecture/Desktop-Shell]].
