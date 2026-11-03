## 2026-11-03 — Young's modulus gate editable in the Monitoring tune panel

The target group's Young's modulus gate (`enable_target_group_emodulus`,
`target_group_emodulus_min/max`, kPa) has been applied by the processing
science, the native core and the PZ7035 PL profile since March, but it was
only reachable through the config file or a profile: the #364 tune panel
listed area and deformability only. `ProcessingConfigDraft` now exposes
`TargetEmodulusEnabled`, `TargetEmodulusMin` and `TargetEmodulusMax`
(JSON `image_processing/target_group/emodulus_*`, 2 decimals), validates
min ≤ max while the gate is on, and the panel shows them as the nested
checkable box `targetEmodulusBox` inside `targetGroupBox`. With an
E-modulus LUT loaded, a lookup outside the table fails an enabled E gate;
with no LUT loaded the E range is skipped and the target group is the
area/deformability box alone (changed later the same day, see
[[2026-11-03-emodulus-gate-needs-lut]]; the first version of this entry, and
of the tooltip, said such a gate admits no cell). See [[ExperimentMonitoringTab]]
and [[System-Utilities]].
