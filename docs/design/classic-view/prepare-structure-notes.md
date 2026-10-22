# Prepare structure, for the O1b design and its implementation

Collected from the code at b1b6aff7 by the landing agent (#794), 2026-10-22.

## Inputs
- `report = derivePreflight(preflightInput)`. Each check has id, label, requirement (required | optional | not-applicable), status (passed | warning | failed | unknown | unsupported | not-required), detected, expected, detail, and blocks (true when a required check failed, is unknown or warns).
- Backend gates come from `readiness.snapshot.gates` (id, status, reason, remediation), read without a path every 2 s. The calibration controller and gate, the Z stage status and the confirmation signature are also inputs.

## Rows by checklist id
| Row | Checklist ids |
|---|---|
| Camera and acquisition | camera, core, capture, trigger; on the PZ7035 also plCore, sensorLink, led |
| Focus | autofocus, plus the Z stage via zStageFact (unsupported on the PZ7035) |
| Fluidics | samplePump, sheathPump |
| Data destination | storage |

"Optional, not connected" means status not-required with requirement optional. On the mock camera that is autofocus, both pumps, trigger, capture (until the camera runs) and the Z stage when it reports not connected.

## The detected line
Built from:
- camera.detected (e.g. "Mock camera");
- core.detected and core.status (e.g. "0.3.2", "Trusted core is active and pinned");
- on the PZ7035, plCore.detected.

## Gates by subsystem (labels in GATE_LABELS)
| Subsystem | Gates |
|---|---|
| Camera and acquisition | camera.* (session, source, deliveryMode, geometry, mode), instrument.* (mode, strobe), science.pl, ring.placement, run.frozen, processing.core, processing.profileCompile, telemetry.transportLoss, trigger.output, rf.generator |
| Calibration | calibration.record, calibration.pixelToMicron, and the method report (methodCalibrationOf) |
| Data destination | storage.*, lifecycle.recording |

- **storage.output** is always Unavailable in Prepare ("no output path chosen yet"). It must never count as an issue.
- **No hardware row:** processing.roi and processing.background (on the mock camera these warn until Align and Set Background), processing.config, processing.backgroundCalibration, method.revision, clock.wall, lifecycle.experiment, lifecycle.fault. These need a visible home, either under the verdict or on a "Method / Align" line, so they can never be hidden.

## Verdict words
`prepareVerdict` returns one of:
- Not ready
- Readiness paused
- Ready to confirm, or Confirmed (each can add "· Start blocked" or "· gates not read yet")
- Ready with warnings
- Ready

The header chip and the Plan use these words too, so derive a new headline alongside them; don't replace them. On the mock camera with the camera idle, the checklist passes, but camera.session and camera.source fail and three gates are unknown. The issue state therefore comes from the gates, not the checklist. The workspace's camera.session Fix is in GATE_REMEDIATIONS.

## Confirm
- It depends on the checklist only (confirmBlockers), plus a lock when there is no camera or a run is active.
- It is advisory and kept per tab.
- startRefusalText explains a Start that would still be refused.

## Calibration auto-expand
It expands for any of: the calibration.record gate status; methodCalibrationOf(snapshot)?.purpose === "calibration"; or a proposed record in list.records. fixAction(gate, list) provides the Re-apply beside the Fix.

## Disclosure
- @mib/ui has no disclosure component, so use a native <details> styled with kit tokens. Give it a controlled summary click, so a user's open can be told apart from an automatic one.
- The "no failing gate hidden" test must check that each failing gate's text sits outside any closed <details>, because hidden content still counts in textContent.
