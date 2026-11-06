## 2026-11-05 — Tauri camera actions, mock dialog and the remaining Connect items (#863 B2)

Start and Stop Camera follow Qt's `CameraController`: one command in flight (a duplicate click, key or shortcut
sends nothing), the camera's phase beside the buttons ("Camera starting...", "running", "stopping...", "failed",
"not configured"), a refused or failed command is a modal warning titled "Camera", Stop is refused while an
experiment is Starting, Active or saving its data (backend and buttons), a device teardown that was not
confirmed is a failed Stop with the camera's message, and the space bar toggles the camera through the same
guarded commands. Configure Mock Camera takes the frame rate in fps (1 to 10000, 5000 by default) through the new
`configure_mock_fps` (whole-microsecond interval; bridge ABI stays 45) and starts from the last folder used, else
the backend's mock folder, else Documents. The preflight now warns when the connected camera is not the active
profile's `camera.window.camera_model` and checks the data directory's storage. The focus gate's staleness
(`ring_ratio_stale_ms`) is #883's (HW-034, `afStaleMs`); this PR leaves it. Decisions (raw recording on Stop, display rate, `experimentCompleted`) are in
[[architecture/Desktop-Shell]]. See also [[architecture/Rust-Bridge]], [[architecture/AppBackend]].
