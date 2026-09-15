# Phase 4 remaining slices — plan & verification guide

Status: active (2026-07-15). Companion to
[`2026-07-15-qt-decoupling-and-tauri-migration.md`](2026-07-15-qt-decoupling-and-tauri-migration.md).
Tracks epic #246 Phase 4.

## Where we are

The React + Tauri desktop app (`desktop/`) drives the Qt-free C++ backend through
the `mib-bridge` cxx bridge (ADR 0003). Landed and merged on `dev/react-tauri`:

- **Slice 1 — recording + review** (bridge schema v2): record the live stream to
  HDF5, load it back, scrub by frame index.
- **Slice 2 — processing settings + stats** (bridge schema v3): realtime toggle,
  pixel→micron, live fps overlay.
- **UX** — native file-picker dialogs (`tauri-plugin-dialog`).

All of the above are verified **headless** (`cargo test` bridge round-trips +
`desktop/scripts/xvfb-smoke.sh` GUI smoke) in `bridge-ci.yml` / `desktop-ci.yml`.

## The verification boundary

Every remaining Phase 4 workflow is either **hardware-dependent** or **large and
mostly visual**, so it cannot be verified end-to-end in the headless CI
environment the way slices 1–2 were. Each slice below lists what *can* be
verified headless (via mock/fake seams) and what needs a real device or a human
looking at the screen. The pattern is unchanged: extend the bridge (bump
`bridge_abi_version()` additively), add Tauri commands, build UI, add a headless
`cargo test` over the mock/fake path, keep the Xvfb smoke green, PR into
`dev/react-tauri`.

## Slice 3 — camera selection (hardware + MindVision)

**Goal:** choose a real camera (EGrabber interface/device index, or a MindVision
index + config) instead of only the mock folder.

- **Bridge:** the facade already has `CameraCommand` actions
  `SelectHardwareCamera`, `SelectMindVisionCamera`, `ApplyCameraScript`,
  `ResetSelectedHardwareCamera`. Expose them as bridge commands
  (`select_hardware_camera(iface, dev, label)`, `select_mindvision_camera(index,
  label, config_path)`, `apply_camera_script(path)`, `reset_hardware_camera`).
- **UI:** a Camera panel — mode radio (mock / hardware / MindVision) + the index
  fields + a config/script file picker.
- **Headless-verifiable:** the mock path (done). On non-Windows,
  `setHardwareCameraSelection` intentionally falls back to mock and stays
  `configured`, so a `cargo test` can assert the command dispatches and the
  camera reports configured — but **no real frames** without a device.
- **Needs hardware:** actual EGrabber/MindVision capture. Validate on a Windows
  box with the SDK + camera (mirror `tests/hardware/hw_camera_test.cpp`, gated by
  `MIB_TEST_CAMERA`).

## Slice 4 — syringe pump

**Goal:** connect a dLSP pump over serial, set flow rate, poll status.

- **Bridge/facade:** `SyringePumpService` is **not** in `BackendCommand` yet —
  add a `PumpCommand` variant to the facade (connect(comPort, baud), setFlowRate,
  pollStatus, disconnect) and expose it through the bridge. Keep the service's
  `setSerialPortFactory` seam.
- **Headless-verifiable:** inject a `FakeSerialPort` (the Modbus-slave fake from
  `tests/backend/syringe_pump_fake_serial_test.cpp`) and drive connect →
  setFlowRate → pollStatus over it. To reach this through the bridge, add a
  **test-only** factory-injection seam on `BackendBridge` (feature-gated) or test
  the facade command directly in a backend test. Prefer the latter: a
  `backend/` test that dispatches the new `PumpCommand`s against a fake port —
  keeps the bridge surface production-only.
- **Needs hardware:** a real pump on a real COM/tty port.
- **UI:** a Syringe Pump panel — port + baud + flow-rate input + status readout.

## Slice 5 — autofocus / nanopositioner

**Goal:** drive the nanopositioner and run the ring-ratio autofocus sweep.

- **Bridge/facade:** add facade commands for `AutofocusService` (start/stop
  sweep, move to position, read focus metric). The service already receives a
  ring-ratio callback from `ProcessingService`.
- **Headless-verifiable:** limited — the autofocus *logic* can be unit-tested,
  but the positioner is hardware. A stub positioner (mirror the
  `AutofocusService.stub.cpp` precedent) lets a `cargo test` drive the state
  machine without motion.
- **Needs hardware:** the real nanopositioner for closed-loop focus.
- **UI:** a Nanopositioner panel — jog controls + a "run autofocus" button +
  focus-metric plot.

## Slice 6 — experiment run + monitoring

**Goal:** the core deformability-cytometry run: start/stop an experiment,
accumulate validated objects, show live monitoring charts, export.

- **Bridge/facade:** the biggest addition — the "experiment" is a frontend
  concept over `ProcessingService` accumulation + `Hdf5Service` export, so add
  an `ExperimentCommand` set to the facade (start(config), stop, export) plus an
  `ExperimentStatusEvent` (frames processed, valid/invalid counts, run state).
- **Headless-verifiable:** the run **lifecycle + accumulation counts** via the
  mock camera (a `cargo test`: start → let frames flow → assert counts advance →
  stop → export → assert the HDF5 exists). This is genuinely testable.
- **Needs a human:** the live scatter/histogram charts (deformability vs. area,
  Young's modulus) are visual — verify by eye, and against the Qt app's output
  for parity.
- **UI:** an Experiment panel + a Monitoring view (charts). Consider a charting
  lib that satisfies the CSP (bundled, no CDN).

## Suggested order

1. **Experiment run (slice 6)** — highest user value, and its lifecycle is
   headless-testable even though the charts aren't. Do the facade
   `ExperimentCommand`/`ExperimentStatusEvent` + a headless lifecycle test first,
   then the charts.
2. **Camera selection (slice 3)** — unblocks real-hardware use; ship the
   commands + UI, validate capture on a Windows+SDK box.
3. **Syringe pump (slice 4)** — self-contained, fake-serial-testable.
4. **Autofocus (slice 5)** — most hardware-bound; do last.

## Guardrails (unchanged from ADR 0003)

- Extend `BackendCommand`/`BackendEvent` variants; bump `bridge_abi_version()`
  additively — never repurpose a field.
- Frame/blob payloads stay on the binary `frame_bytes` channel — no per-frame
  base64/JSON.
- Keep the event sink non-blocking on backend threads.
- Every slice: headless `cargo test` over the mock/fake seam + green Xvfb smoke,
  plus matching vault updates (`Vault-Maintenance.md`).
