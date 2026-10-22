## 2026-10-22 — Native eGrabber workspace qualification and path-less readiness (#806, #713)

Extended `backend.readiness_probe_cache` to prove Prepare's empty-path polling
writes neither to the working directory nor a previously selected destination,
with both cold and warm probe caches. Directory timestamps detect even probes
that create then delete their files; storage remains Unavailable and no HDF5
round-trip gate is invented. Capture bootstrap is explicitly disabled.
See [[architecture/ExperimentCoordinator]].

The dedicated workspace build passes 24 native CTest cases covering discovery,
capture lifecycle, buffer bounds/reserve/concurrency/resize, owned cell images,
readiness, HDF5 fault recovery/export, provenance and accounting round trips,
and pipeline stress. These run real native services with synthetic inputs and
simulated providers, not an EGrabber device or Windows SDK qualification.
The E ledger records reproducible commands and separate transport-level evidence.

Do not use pending shared-contract work as a stop condition for independent
implementation or validation. Continue authorized work without owner check-ins;
retain external ownership and the hold on promotion/deployment/hardware actions.

A separate `crates/mib-bridge-server/tests/egrabber_buffer.rs` exercises the actual
WebSocket command route and mock-camera/native FrameStore: active resize refused,
stopped range export produces files, destructive shrink without confirmation
preserves frames, confirmed shrink succeeds, stale generation is rejected, and
capture stays stopped. Bounded polling waits for observed frames rather than a
fixed startup delay. This tests exported-file presence, not independent TIFF
pixel decoding or physical camera behavior. No shared bridge test file changed.
