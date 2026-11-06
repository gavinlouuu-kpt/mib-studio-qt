## 2026-11-07 — The launch camera auto-start waits for discovery probes (#905)

On the MIB rig's first launch of 1.2.0-beta.d9d74fee the camera did not start by itself. The
startup discovery job (job 2) selected the camera and navigated to the Overview tab while the
Connect tab's launch refresh (job 1) was still probing the same Coaxlink device. A camera probe
holds the device for the length of the provider call (`EGrabberDiscovery` in
`CameraControlService::discoverCameras`/`discoverFramegrabbers`), and the discovery service's
`camera-sdk` lock serializes probes only with each other. So the Overview auto-start's open failed
with "operation not allowed", the camera script after it failed the same way, and the operator had
to press Start Live View. The second launch worked because the auto-start came 3 ms after the last
probe.

`MainWindow::onTabChanged` now runs its camera step (the Overview auto-start and the camera script
for Overview/Experiment, `applyTabCamera`) only when no camera or framegrabber discovery job is in
flight (`DeviceDiscoveryService::hasActiveJob`). Otherwise it logs "camera discovery in flight; the
Overview camera step waits for it" and a 100 ms timer runs the step when the jobs finish (bounded at
15 s). A tab change, a window close or a run starting meanwhile drops the waiting step. Discovery
itself is unchanged: the Connect tab still lists devices from its own refresh and from the startup
job. See [[frontend/MainWindow]] and [[services/DeviceDiscoveryService]].

Test: `frontend.camera_autostart_discovery` (a real `MainWindow` on a mock camera that refuses to
open while a fake `camera-sdk` provider holds it). It fails on the old code with the rig's sequence
(auto-start, refused open, `CaptureService camera failed to start`).
