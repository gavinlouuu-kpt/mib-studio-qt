## 2026-11-03 — Target group: the Young's modulus layer applies only when an E table is loaded

Decision (Gavin): "When no E is loaded just sort with the rectangle so it's like
two layer filter." A valid cell with `enable_target_group` on is target group
when the area box AND the deformability box hold AND the E layer passes, where
the E layer passes if `!enable_target_group_emodulus`, OR no E-modulus LUT is
loaded, OR (E is not NaN AND `emodulus_min` ≤ E ≤ `emodulus_max`). With a LUT
loaded a cell whose lookup is NaN (outside the table) still fails the layer.

Why: without a LUT `FilterResult::youngsModulus` keeps its `0.0` default
(`ProcessingTypes.h`), which is a placeholder, not an E. The old expression
tested it against the range, so with the default `emodulus_min` 0 every cell
passed by accident and with a positive minimum no cell was ever target group;
the Monitoring overlay and count meanwhile said "admits no cell" (see
[[2026-11-03-monitoring-gate-overlay]], [[2026-11-03-tune-panel-emodulus-gate]]).

What moved:
- **One rule, one function.** `science::isTargetGroupMember`
  (`ProcessingScience.h/.cpp`) replaces the three copies of `tgEmod` (inner
  contour, outer contour, U-Net cell) and is what the ABI v2 loader calls.
  [[ProcessingService]].
- **Native core loader** (`ProcessingCoreLoader.cpp`). The core has no LUT. The
  host already recomputed E and the target group when a LUT was loaded; without
  one it used to take the core's `is_target_group` as is. It now recomputes the
  target group from the box for every valid cell, so a core built before this
  rule gives the same answer as the bundled science. The Contract-2 contract
  (core runs the same `filterProcessedObjects`, host applies the LUT) is
  unchanged: the in-repo core calls `filterProcessedObjects` with no LUT and now
  answers "box" instead of "false" for an enabled E gate, which the host
  overrides when a LUT is loaded.
- **PZ7035 profile** (`PzProfileCompiler.cpp`). No table with the E gate on was
  a compile error that stopped the run from arming. It now compiles with
  `target_group_emodulus_enabled` clear (the PZ7035 ABI already forbids
  enabling E sorting without a table) and adds a warning to
  `CompiledProfile::warnings`; the `processing.profileCompile` readiness gate
  shows it as a Warn, not a Fail. The E range words are still written.
  [[PZ7035-Records]].
- **Monitoring scatter** ([[ExperimentMonitoringTab]]). `gate::isTarget`,
  `wouldBeTarget`, `computeMask` and `countTargets` take the table state; with no
  table the tint is the box, no contours or E handles are drawn, the live count
  is the box, and the red pill reads "No E-modulus table loaded: the target group
  uses the box only". The tune panel tooltip of `targetEmodulusBox` says the
  range is skipped without a table.
- **Docs.** `docs/gold_standard_metrics.md` (`target_group.emodulus_enabled`),
  `docs/manual/acquire-and-record.md`.

Not changed: the config schema and defaults, reanalysis of old recordings (they
follow the same rule when reprocessed), and `RunStatistics` (it still reports
`emodulus_gated` from the recorded config, not whether a table was loaded).

Guards: `processing.target_group_emodulus` (the rule on masks of all three
contracts, zero and positive minimum), `processing.core_contract2_equivalence`
(through the real core, with and without a table), `processing.pz_profile_compiler`,
`frontend.target_gate_preview`, `frontend.monitoring_gate_overlay`.
