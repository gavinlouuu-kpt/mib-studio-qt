## 2026-11-02 — #810 (FlowJo analysis) merged onto the workspace branch (fold-in slice U-2a)

Slice U-2a of the fold-in realignment plan
(`docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md`, decision D2): branch
`claude/810-on-workspace` starts at the U-1 branch (`feat/egrabber-workspace` with `develop`
merged) and merges `feat/808-flowjo-realignment` (#810). F owns `src/backend/analysis`,
`crates/mib-analysis-worker`, the review analysis bridge and the contract text, so the result
carries F's analysis code unchanged at **bridge ABI 47** (45 reserved for #803, 46
`population_analysis`, 47 `analysis_workspace`) and **review ABI 9**. The workspace branch was at
ABI 44 / review 6 with no numbers of its own above that, so nothing was renumbered.

One conflict: `desktop/src/review/ReviewPanel.tsx`. The workspace branch had replaced the
"Images not recorded" note with a `Notice` and the sub-tab bar with the inspect body; #810 only
added the `PopulationAnalysis` panel above that note. The resolution keeps the workspace layout and
mounts `PopulationAnalysis` (experiment files, disabled while a review job runs) above the notice.
No goal row (F01–F16) changes state with this merge; see [[services/AnalysisWorker]] and
[[frontend/YofoReview]].

#810 left two desktop version pins behind its contract (`eventAdapter.test.ts` and
`runQuickLook.test.ts` expected bridge ABI 46 / review ABI 8 while the generated contract says
47 / 9); they fail on `feat/808-flowjo-realignment` itself and are updated to 47 / 9 here.

Two Rust tests failed on #810 itself and pass on the U-1 branch: `mib-app-commands`
`monitoring_cells_through_dispatch` and `mib-bridge-server` `live_cells_over_the_websocket`.
#810's facade `stop_capture` now calls `ProcessingService::drainAndStopRealtime()` after
`capture().stop()` (so the live evidence summary is finished), and that realtime stop reported a
second monitoring capture change, "realtime processing stopped", on top of the camera close's
"the camera stopped" — the refusal and the ended pins named the wrong reason. The drain/stop stays;
`drainAndStopRealtime(false)` skips the second change when an active camera's stop already ended
the capture (with no active camera the realtime stop still ends it). See
[[services/ProcessingService]] and [[services/MonitoringCellStore]].
