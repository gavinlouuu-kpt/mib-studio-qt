# Isolated E02 acceptance

No device is opened by these targets. No proprietary SDK files are vendored.

```sh
cmake -S tools/egrabber_acceptance -B build/egrabber-acceptance
cmake --build build/egrabber-acceptance -j4
ctest --test-dir build/egrabber-acceptance --output-on-failure
```

Optionally configure `-DEGRABBER_INCLUDE_DIR=/path/to/sdk/include` for real SDK
compile-only instantiation plus fake-grabber adapter tests. Verified with bundled
Linux eGrabber 26.09.0.5 headers. This is not Windows/runtime/hardware proof.

Core checks cover stale identity, dynamic access, exact scalar types, numeric
bounds and increments (including int64 extremes), enums, string/address rejection,
readback discrepancy and unknown write outcomes. Adapter checks exercise all five
module dispatches, SDK selector-query composition and refusal when rollback or
range checks are disabled. No fake result implies physical selector restoration.

`egrabber.feature_tree` (E-1) checks node-map enumeration and guarded typed
access over a fake node map with generated names: the returned tree equals the
node map exactly, a session change mid-walk is stale, SDK failures stay per node,
writes advance the capability revision and access is re-read. With the SDK
headers the adapter test also checks the composed enumeration query strings.

Integration (E-2) lives in the main build: `CaptureService::withCameraFeatures`
serializes token acquisition through readback with stop and the trigger thread,
and bridge ABI 49 exposes `camera_feature_tree/read/write` (see
`knowledge_map/camera/FeatureControl.md`). Multi-selectors, commands, physical
qualification and final E02 acceptance (rig tree equals the vendor tool's tree)
remain outstanding.

The path-filtered `egrabber-feature-control.yml` workflow runs the SDK-free core
with UBSan on pull requests. SDK adapter checks remain explicitly local because
the SDK is not vendored or provisioned by CI.
