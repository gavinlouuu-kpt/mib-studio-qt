# 2026-09-14 — One-click illuminated Live View

Issue: [#413](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413)
Implementation: [PR #414](https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/414)

## Request and result

Replace repeated camera Apply, capture Play and generator Connect/Start with
one saved rig setup and coordinated Play/Stop. Implemented opt-in XGC/R5D
preset, capture-owned generator/strobe sequencing, readback gating, manual
control exclusion, shutdown failure reporting and a three-second frame-stall
fault. Exposure remains visible and advanced setup is collapsed.

All changes are isolated from the dirty shared checkout. No hardware was
started, routing changed or deployed application replaced during this task.

## Evidence

Baseline regression reproduced missing generator coordination. Backend/full
Qt builds, software regression suite, Qt setup/persistence tests and TSan
checks passed locally. Hardware validation is not inferred from those tests.
The historical September 10 5 kHz/64.6 µs current result supports the initial
preset, not new-build physical acceptance or exact exposure edges.

## Next release step

PR review/CI and deployment, then Rigol commissioning of the changed build.
Daily Play/Stop does not require repeating commissioning. See the
[operator guide](../../docs/howto/illuminated-live-view.md) and
[completed execution plan](../../docs/exec-plans/completed/2026-09-14-one-click-live.md).


### Everyday FPS adjustment

Requested FPS is visible beside Exposure for a saved illuminated rig. Stop capture,
change FPS, Save, and Play to apply it through the coordinated generator startup.
It edits `live_view.frequency_hz`, not the camera's free-running speed selector.
The generator supports 400–40000 Hz; this is not a camera throughput guarantee.
The bench-tested point is 5000 FPS at 512×96. Observe actual acquisition rate and
use Rigol for physical timing acceptance when commissioning another rate.

Changing FPS preserves the trigger's active duration by scaling saved duty with
frequency: the preset's 5000 Hz / 10% becomes 2500 Hz / 5%, retaining a requested
20 µs trigger pulse. Exposure and strobe width/delay are not silently changed.
Existing backend validation rejects exposure or strobe timing that exceeds the
new period, and invalid generator duty. Legacy/manual profiles leave FPS disabled.
The real-widget regression covers visibility, persistence, duty compensation,
unchanged exposure/strobe, and restoration after reopening.
