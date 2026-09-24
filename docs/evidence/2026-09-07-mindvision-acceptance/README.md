# MindVision hardware acceptance — reliability release #371

Hardware-host acceptance for branch `claude/host-sdk-reliability-qt-ui-g03ubd`,
starting at `064ba62`, on 2026-09-07.

## Environment

- Host: Ubuntu 24.04.3 LTS, kernel `6.17.0-1032-oem`, Qt 6.4.2.
- Camera: MindVision MV-XG51GM, serial `056061120465`, GigE camera index `0`,
  camera `169.254.106.241` via host interface `enp133s0f0` at
  `169.254.100.1`.
- SDK: pinned Linux MindVision MVSDK provisioned with
  `scripts/provision-mindvision-sdk.sh`; runtime version `2.1.0.49`.
- Camera environment for hardware commands: `MIB_TEST_CAMERA=1`,
  `MIB_CAMERA_MODE=mindvision`, `MIB_MINDVISION_CAMERA_INDEX=0`; no
  `MIB_MINDVISION_CONFIG` was supplied unless a row says otherwise.
- Frontend automation: `QT_QPA_PLATFORM=offscreen` and system Qt 6.4.2.
- Bench serial state: all USB serial adapters connected. A running
  `its209_supervisor.py` process held bench serial devices during the frontend
  runs. The supervisor was not stopped because doing so would interrupt a
  separate active bench service.
- Platform limitation: this is a Linux host, so Windows Release, exporter EXE,
  installer, and installed-package gates could not run.

## Acceptance results

| Gate | Environment and command | Result | Evidence |
|---|---|---|---|
| Frontend lane on MindVision bench | Real MindVision SDK installed; all USB serial adapters connected; `its209_supervisor.py` active. `PYTHONPATH=/tmp/reliability-371-pydeps QT_QPA_PLATFORM=offscreen MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL=file:///nonexistent/mib-lut-manifest.json ctest --test-dir build/linux-system --output-on-failure -L frontend` | First run: **15/19**; `frontend.ui_layout`, `frontend.config_tabs_state`, `frontend.preview_layout`, and `frontend.run_status_ui` stalled. Watchdog output was lost when `LastTest.log` was overwritten. Cause unknown. The stall did not reproduce across 12 targeted reruns (three per affected test), a full-lane rerun (**19/19 in 38.5 s**), or the final session rerun (**19/19 in 37.55 s**). This does not establish a root cause or retroactively make the first hardware-host run green. An idle-supervisor rerun was **not run** because the active ITS209 bench service could not be interrupted unattended. | [final console log](logs/ctest-20260907-160018-frontend-final-console.log), [final `LastTest.log`](logs/ctest-20260907-160018-frontend-final-LastTest.log) |
| 1. `hardware.camera` | `MIB_TEST_CAMERA=1 MIB_CAMERA_MODE=mindvision MIB_MINDVISION_CAMERA_INDEX=0 ctest --test-dir build/linux-backend -R '^hardware\.camera$' --output-on-failure --timeout 60` | First acceptance rerun **failed**: zero frames before the test's fixed 5 s deadline; the camera reported started just after the deadline and then stopped cleanly. Immediate exclusive rerun **passed 1/1 in 6.17 s**. This exposes startup-time sensitivity in the fixed 5 s assertion. | [first run](logs/ctest-20260907-152148-hardware-camera.log), [passing rerun](logs/ctest-20260907-152217-hardware-camera-rerun.log) |
| 2. Fail-closed conversion and mid-session geometry change | Deterministic guard: `ctest --test-dir build/linux-backend -R '^backend\.mindvision_conversion_fault$' --output-on-failure`. Requested real run: non-Mono8 config plus SDK-tool ROI/resolution change during capture. | Deterministic guard **passed**. Real-hardware fault injection **not run**: `MindVisionConfig` exposes no ISP-output-format key, and this Linux setup has no safe supported hook/vendor tool to mutate the active camera's frame format or geometry through the app-owned handle. After debugger capture the SDK also rejected new opens with AIA `0x8006` (`32774`, access denied) pending a physical camera reset. | [guard log](logs/ctest-20260907-153828-acceptance-guards.log), [SDK reopen failure](logs/mindvision-sdk-reopen-20260907-155647.log) |
| 3. 50-cycle lifecycle stress | Temporary acceptance harness (source preserved below) instantiated the real `AppBackend` and `CameraController`; command: `timeout --signal=TERM --kill-after=10s 900s /tmp/mindvision_lifecycle_50`. It performed 50 `requestStart()`/`requestStop()` cycles against MindVision index 0. Five stops were issued 1 ms after `Running`; every later cycle waited for a new frame. | **Passed**: generations 1–50 were consecutive; final state `Idle`; all 50 stops completed in 148–160 ms (mean 153.4 ms); no hang or `std::terminate`. Logs show `TriggerService stopped` 50/50 times before MindVision teardown. The first five immediate stops are strong timing evidence but the private in-flight SDK counter was not instrumented, so “definitely in flight” is not claimed. | [harness source](logs/mindvision_lifecycle_50.cpp), [full log](logs/mindvision-lifecycle-50-20260907-152826.log) |
| 4. Diagnostics timestamp and telemetry truth | Requested live Diagnostics inspection with the real camera; deterministic guard `ctest --test-dir build/linux-backend -R '^backend\.timestamp_telemetry$' --output-on-failure`. | Deterministic guard **passed**. Real live Diagnostics result **not run**: after the debugger capture, `CameraInit` consistently returned `32774`/AIA `0x8006` and no process owned the camera locally; a physical camera reset was unavailable during the unattended session. Therefore exact MindVision UI strings and real metric validity were not claimed. A debugger trace from an acceptance helper was saved; it showed an offscreen modal caused by launching the temporary executable outside the build tree (`OverviewTab::onReloadJs()` → `QDialog::exec()`), not serial enumeration or layout re-entry. | [guard log](logs/ctest-20260907-153828-acceptance-guards.log), [camera-open attempt](logs/mindvision-diagnostics-20260907-153919-rerun.log), [debugger trace](logs/mindvision-diagnostics-gdb-20260907-153708.log) |
| 5. Readiness and run-provenance snapshot | Deterministic guard: `ctest --test-dir build/linux-backend -R '^backend\.experiment_readiness$' --output-on-failure`; requested live experiment plus disconnect/ROI mutation between preflight and Start and `h5dump -A -g /run_provenance <run.h5>`. | Deterministic guard **passed**. Real-hardware UI/HDF5 gate **not run**: the camera remained SDK-access-denied pending a physical reset, and the app exposes no automation hook that pauses between preflight and serialized Start for safe unattended mutation. | [guard log](logs/ctest-20260907-153828-acceptance-guards.log), [SDK reopen failure](logs/mindvision-sdk-reopen-20260907-155647.log) |
| 6. 30-minute acquisition/recording soak and low-memory policy | Requested app recording for ≥30 min, five-minute Diagnostics samples, accounting inspection with `h5dump -A -g /experiment_info <run.h5>`, then a profile run with `experiment_buffer_max_mb=32`. Deterministic guards: `processing.memory_budget` and `performance.memory_budget`. | Deterministic guards **passed**. Real-hardware soak and low-memory recording **not run**: the camera remained SDK-access-denied pending a physical reset; no duration-controlled real-hardware recording harness exists; no operator-confirmed real sample or moving target was available. No 30-minute or low-memory HDF5 artifact was produced. | [guard log](logs/ctest-20260907-153828-acceptance-guards.log), [SDK reopen failure](logs/mindvision-sdk-reopen-20260907-155647.log) |
| 7. EveryFrame versus LatestFrame | Deterministic guards: `ctest --test-dir build/linux-backend -R '^camera\.delivery_mode_(contract|overload)$' --output-on-failure`; requested live mode changes through the Connect tab with Diagnostics/accounting inspection. | Deterministic guards **passed**. Real-hardware comparison **not run**: the camera remained SDK-access-denied pending a physical reset, and `hardware.camera` has no delivery-mode/overload argument. No claim is made about real MindVision intentional-discard accounting or `IntentionallyPartial` classification. | [guard log](logs/ctest-20260907-153828-acceptance-guards.log), [SDK reopen failure](logs/mindvision-sdk-reopen-20260907-155647.log) |
| 8. Exporter on Windows | Requested `python scripts/exporter_soak.py --cycles 50 --mode gui` and native `recording.hdf_export_soak` with `MIB_EXPORT_SOAK_CYCLES=50` against a hardware recording. | **Not run:** host is Linux, no Windows/PyInstaller executable was available, and the blocked recording gate produced no hardware `.h5` input. Existing cross-platform exporter evidence remains in `docs/evidence/2026-09-07-exporter-soak/`; it is not substituted for this Windows acceptance. | Platform/environment record above. |
| 9. Packaged executable | Requested Windows installer build/install followed by gates 1, 4, and 5 and unplugged-camera startup. | **Not run:** host is Linux and cannot build/install or exercise the Windows package and `MVCAMSDK_X64.dll`; no EGrabber hardware is present either. | Platform/environment record above. |

## Camera access state at handoff

The 50-cycle lifecycle test completed cleanly and released the camera. A later GDB
capture intentionally terminated an acceptance helper while it was constructing the
UI. Subsequent SDK probes enumerated the MV-XG51GM but `CameraInit` consistently
returned `32774` (`0x8006`, AIA access denied), with no local camera-owning process.
The Ethernet route and ping remained healthy. Further real-hardware gates require a
physical camera power-cycle/reset followed by a bounded SDK open/uninitialize probe.
