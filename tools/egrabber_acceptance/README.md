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

Integration must retain existing CaptureService/camera ownership and serialize
session-token acquisition through readback with stop, scripts and selector access.
No bridge ABI or UI entry point is added. Tree enumeration, multi-selectors,
commands, physical qualification and final E02 acceptance remain outstanding.
