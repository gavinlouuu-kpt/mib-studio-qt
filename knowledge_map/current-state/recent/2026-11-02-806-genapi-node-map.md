## 2026-11-02 — GenApi node-map enumeration and wired typed control (#806 E-1/E-2, E02)

The camera's own GenApi node map now decides what exists and what is writable.
`Port` gained per-module enumeration (categories, child categories, features,
node info); `EGrabberFeaturePort` implements it with the SDK's `query::categories`,
`categoriesOf`, `featuresOf`, `features` and `info` (verified against the SDK
header, not on hardware). `camera::features::enumerate` walks the tree one
guarded node at a time under `CaptureService::withCameraFeatures` (capture guard,
then the camera's port guard), carrying the session token {source, capture
generation, capability revision}. `ICamera::featureSession()` is the optional
capability (nullptr = unsupported); `EGrabberCamera` serves the grabber under
`triggerMutex_`, `MockCamera` a fake node map (evidence fixture/mock). Bridge
ABI 49: `camera_feature_tree`, `camera_feature_read`, `camera_feature_write`
(control). Stale generations and revisions are refused; requested, readback,
refused, unsupported and unknown outcome stay distinct. The features namespace
moved from `backend::camera::features` to `camera::features` (beside
`camera::common`) so `ICamera.h` does not shadow `camera::` lookups inside
`namespace backend`. No hardware run; no UI change. See [[camera/FeatureControl]],
[[architecture/Rust-Bridge]].

**Review fixes (E02, E01).** (1) Feature I/O could land between a sort pulse's two
`setTriggerOutput` edges and stretch it. Every feature operation (tree, read,
write) is now refused, with outcome `refused` and a reason and without touching
the device, while the sort trigger is armed or running
(`TriggerService::runWhileTriggerIdle` decides and runs under `pulseMutex_`, the
lock held across a whole pulse) and while an experiment is Starting, Active or
Stopping. In the default boot the trigger runs with every capture session, so
the commands answer `refused` there until the trigger service is disabled. Only
a write (or selector-scoped describe) on the Interface module now clears
`triggerLineApplied_` (`ModuleWriteTracker`). (2) The tree is a facade job; the
Rust bridge lock is held only for its start and each fetch; one walk in flight
(identical requests join, others are refused). The cxx surface changed within
the unmerged ABI 49 (`camera_feature_tree_start` / `_fetch`); the WebSocket wire
format did not. (3) The run-state gate (`CameraFeatureGate`) is evaluated under
the camera guard; writes run inside the coordinator's idle transaction and are
also refused during realtime processing and autofocus. (4) An absent module is
`enumerated: false` with reason `Absent: ...` (`ModuleAbsent`). (5) New tests:
`camera.trigger_feature_interlock` (contention stress), `e2e.camera_feature_facade`
(gates through `BackendFacade`), server `feature_io_refused_while_the_sort_trigger_is_armed`.
See [[services/TriggerService]], [[services/CaptureService]].
