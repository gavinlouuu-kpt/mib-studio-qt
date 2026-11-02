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
