# Aravis framework first slice

Status: first slice implemented; PZ7035 hardware integration pending

The optional Aravis consumer is now integrated behind `MIB_ENABLE_ARAVIS`.
The bounded first slice is a real Fake-interface consumer for copied Mono8
preview frames; it does not claim PZ7035 driver, GenTL producer, SSD, or
full-rate recording support.

## Validation

- Local Aravis 0.9.3 / `aravis-0.10` was built from the revision in
  `env/aravis.toml` with USB, packet-socket, viewer and GStreamer disabled.
- The ON build was configured and built in `/tmp/mib-aravis-system-build2`:
  `PATH=/usr/bin:/bin PKG_CONFIG_PATH=<local-prefix>/lib/x86_64-linux-gnu/pkgconfig`
  plus `-DMIB_ENABLE_ARAVIS=ON -DMIB_BUILD_BACKEND_ONLY=ON -DBUILD_TESTING=ON`
  and `-DCMAKE_IGNORE_PATH=/home/linuxbrew/.linuxbrew`.
- The consolidated `mib_backend_tests` target builds successfully. Focused
  CTest passed after the lifecycle review fixes:
  `camera.aravis_fake`, `camera.aravis_stress`,
  `backend.aravis_capture_lifecycle`, `backend.aravis_appbackend_source`,
  `backend.capture_lifecycle`, and `integration.aravis_pipeline_e2e` (6/6).
  The application pipeline E2E was also repeated 10 times (10/10), covering
  AppBackend source selection, Aravis Fake capture, FrameStore publication,
  inline realtime processing, retained bytes, capture restart, and shutdown
  while streaming.
  The E2E accounts for the current realtime cursor contract explicitly: the
  initial FrameStore index 0 is not admitted, so the first cycle expects
  `admitted + 1 == committed delta`, with admitted indices 1 through the final
  committed index and zero sequence gaps, store loss, drops, pending, or
  processing failures. HDF5 is disabled in this test, so persistence-pending
  terms intentionally describe the retained in-memory experiment buffer and
  are not presented as SSD/persistence validation.
  The tests cover discovery, configuration, owned-byte copy after more than
  the four configured SDK buffers, running receive timeouts before and after a
  software-triggered frame, overlapping start/stop, concurrent stop/grab/
  restart, CaptureService publication, explicit source selection, and
  propagation of a scripted `aravis.acquisition_stop` failure while host
  cleanup still reaches Idle. Actual Aravis SDK device-fault injection remains
  unvalidated.
- OFF configuration also succeeds and reports `MIB_HAS_ARAVIS=0`; its full
  runner build is currently affected by the host's pre-existing Linuxbrew
  spdlog/fmt header mismatch. This is unrelated to Aravis source selection.
- A TSan build was configured and compiled successfully in
  `/tmp/mib-aravis-tsan-build` with `-fsanitize=thread`, but all four focused
  executions stopped before test code with the host runtime error
  `ThreadSanitizer: unexpected memory mapping`. The focused release stress
  lane is therefore the current concurrency evidence and uses the repository
  watchdog (`_Exit(99)`); rerun TSan in a compatible CI/runtime image.
- This E2E evidence is software-only: it does not validate physical PZ7035
  transport, GenTL, PL-to-Linux-driver ownership, SATA/SSD recording, or UI
  behavior.

## Follow-up

The PZ7035 Linux driver and GenTL producer must define timestamp mapping,
preview ownership and result/recording transport before hardware integration.
