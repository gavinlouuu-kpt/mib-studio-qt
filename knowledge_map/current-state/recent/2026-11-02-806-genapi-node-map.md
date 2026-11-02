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
