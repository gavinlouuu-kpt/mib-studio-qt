## 2026-11-02 — Backend-instance tokens and SFNC quick controls (#806 E-4 then E-3; E01, E02)

**E-4 (E01).** Generations and discovery job IDs restart at 1 in a new backend, so a
token from before a restart could match a new session. `CaptureService` and
`DeviceDiscoveryService` now each draw an `instanceId()` (random UUID v4) at
construction. The feature session token is `{instance, source, generation,
capability_revision}`; a token from another instance is `stale` and never reaches
the port. Discovery start replies and snapshots carry `instance`; `fetch_device_discovery`
and `cancel_device_discovery` take an optional `instance` and refuse another one
(`stale` snapshot, cancel false). Amended into the unmerged bridge ABI 49 (additive for
discovery). Proof: real server `camera_features.rs`
`tokens_from_before_a_backend_restart_are_refused` (two backend instances in turn),
`capture_camera_features_test`, `device_discovery_service_test`, `feature_*_test`, `contract.rs`.

**E-3 (E02 view).** Tauri commands `camera_feature_tree` / `_read` / `_write` and
`bridge.ts` wrappers. Align's exposure, gain, frame rate and trigger mode are bindings
to the SFNC names `ExposureTime`, `Gain`, `AcquisitionFrameRate`, `TriggerMode`, resolved
against the live remote tree (absent, read-only with the access reason, or a draft with
Write); write replies are shown as returned, and the tree is read again after every
write and after a stale reply. The camera ROI still writes through `save_camera_roi`
and shows the node map's `Width` / `Height` / `OffsetX` / `OffsetY`. "Feature tree" opens
a read-only browser of the remote module. The false "All features … stay in the classic
view" pointer is gone. Evidence: fixture/mock only; no hardware run. See
[[services/CaptureService]], [[services/DeviceDiscoveryService]], [[camera/FeatureControl]],
[[architecture/Rust-Bridge]], [[architecture/Desktop-Shell]].
