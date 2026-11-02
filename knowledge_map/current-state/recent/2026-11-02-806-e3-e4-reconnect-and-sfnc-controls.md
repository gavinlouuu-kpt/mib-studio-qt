## 2026-11-02 — Backend-instance tokens (#806 E-4; E01)

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

See [[services/CaptureService]], [[services/DeviceDiscoveryService]], [[camera/FeatureControl]],
[[architecture/Rust-Bridge]].
