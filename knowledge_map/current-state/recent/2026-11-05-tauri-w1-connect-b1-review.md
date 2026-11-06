## 2026-11-05 — Tauri Connect stage B1 re-check against Qt (#863 B1)

Re-verification of the B1 commits found and fixed: the backend refused Configure Mock and the delivery mode in
every non-Idle run state, including `Failed`, while the buttons only looked at Starting/Active/Stopping; the
rule is now `experimentHoldsCamera` (Starting, Active, Stopping). `CaptureService::setDeliveryMode` changes the
mode alone in one step (no read-modify-write of the whole `Config`), and `connect_camera_guards_test` stresses
`setConfig` / `setDeliveryMode` / `config()` against start/stop sessions. The blocked-button texts are Qt's
("... Please stop the camera first.", "Please select a <device> from the list."); Connect and Configure Mock
read the new selection and move to Overview before the device rescan finishes; the startup selection starts
after the first status reading. The delivery-mode choice is not written back to the profile (decision: the
profile is a revisioned file the startup restore is pinned to). See [[architecture/Desktop-Shell]],
[[services/CaptureService]], [[architecture/ExperimentCoordinator]].
