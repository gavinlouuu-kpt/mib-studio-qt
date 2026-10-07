## 2026-10-07 — Trigger/frame alignment integrated with develop (#trigger-frame-alignment)

The feature is adapted to develop’s experiment coordinator, run accounting, setup lock, stage and dot-grid services. The optional `rf.generator` gate is `notRequired` when configuration is missing or disabled, including the bundled defaults. Sorting with an enabled generator retains readback failures and provenance. Bridge ABI remains 31.

Validation: `cmake --build build/linux-system -j8` passed. The three alignment guards passed 20 consecutive runs each. The optional-gate regression failed on both missing config and bundled disabled defaults before the fix; both assertions pass after it, before the readiness test reaches its sandbox-blocked loopback server. The full offline/non-hardware suite ran 225 tests: 212 passed, 7 skipped, 6 failed (RF service/readiness, profile catalog, crash upload timeout, registry HTTP transport, monitoring KDE). The profile-catalog and registry-HTTP failures reproduce at socket/listener creation; monitoring KDE passed its isolated rerun and three consecutive repetitions (its full-suite failure remains unexplained). ThreadSanitizer and Windows were not run in this worktree.

Branch history follows; SSG LAN gate pass, a recorded hardware run and Coaxlink loopback remain rig follow-ups.

### 2026-10-01 — Trigger/frame alignment branch built on the rig PC; SSG link half proven

First Windows build of `feat/trigger-frame-alignment` on the rig
(`windows-ninja`, MSVC): `ScpiTransportVisaWin32.cpp` compiled unchanged.
Three unrelated breaks were fixed on the way — a local named `far` in
`MonitoringDensity.h` (a `<windows.h>` macro), `<dlfcn.h>` in
`processing.core_v2_plugin`, and an out-of-range `1e12` → `int` gate in
`integration.e2e_series_alignment` that left the test with no targets under
MSVC. In the real app with the live Coaxlink camera the `rf.generator` gate
of [[../../services/RfGeneratorService]] blocks Start Experiment with the
expected messages (empty address, wrong IP). The pass case and the recorded
sorting run are **open**: the SSG answers ARP on the PC's `Ethernet 3`, but
that adapter has no address in `10.11.13.0/24` (needs an administrator).
Two rig findings: the `rf_generator` default block is not merged into
**profile** configs (only into `include/config.json`), and the first start
overwrote the profile's `autofocus_initial_voltage` with the parked 0 V.
Evidence and the steps to finish:
`docs/evidence/2026-10-01-ssg-lan-link/README.md`; debt: TD-20, TD-21.

### 2026-09-30 — Sort pulses and multi-image series placed in the frame clock

A sort decision left no trace in the data file beyond `isTargetGroup`, and
multi-image series members were anonymous images. Now
[[../../services/TriggerService]] keeps a canonical, always-on record per
request (`TriggerEventRecord`: source frame + host stamps for request/wake/
fire/done + the real outcome, bounded buffer, `drainEvents`), which the
experiment flush carries into `/trigger_events`; every series member is
appended with its `SeriesImageInfo` (frame index, camera stamp, host stamp)
and a series that skipped frames is flagged `series_contiguous = 0`
(`/valid_frames/series_meta`, see [[../../data-model/HDF5-Storage]]).
[[../../camera/ICamera]] gained `setLineEventCallback` for hardware-stamped
input edges; the trigger service pairs a looped-back pulse edge with its
fired record (FIFO), and [[../../camera/MockCamera]] emulates the loopback so
the whole path runs headless. Guards: `backend.trigger_event_log`,
`recording.trigger_alignment_roundtrip` (round-trip + fault injection),
`integration.e2e_series_alignment` (every fired pulse names a saved series
member; contiguous series are consecutive frames). The sorter itself is now on record too:
[[../../services/RfGeneratorService]] talks SCPI to the SIGLENT SSG3021X
(USBTMC via NI-VISA on Windows / `/dev/usbtmc` on Linux, or LAN 5025),
identifies before any command, reads back trigger mode / delay / width /
RF state, gates a sorting run on an armed instrument (`rf.generator`
readiness gate, [[../../architecture/ExperimentCoordinator]]) and stores the
readback as `rf_generator_*` attributes. Guards:
`backend.rf_generator_service` (fake instrument + real LAN transport over
loopback), the readiness test, the round-trip test. Hardware follow-ups
(Coaxlink loopback of the SSG PULSE OUT into TTLIO11 — wiring decided
2026-10-01 — the `EGrabberCamera::setLineEventCallback` implementation and
the MindVision clock fit) are in
`docs/exec-plans/active/2026-09-30-trigger-frame-alignment.md`; the rig
agent's hand-over with pins, SSG settings, config and acceptance evidence is
`docs/exec-plans/active/2026-10-01-trigger-frame-alignment-rig-handoff.md`.

