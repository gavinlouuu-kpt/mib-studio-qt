## 2026-10-22 — Classic view summary-first: preflight page, left column, Hardware setup page (epic #799)

Gavin's redirect of 2026-10-22: the classic view is the product surface. After testing both views
he chose it ("the user shouldn't see all the hardware config in the first page"), and the boards
were composed in Penpot from the kit and the approved O1b content. Three tasks, one branch:

- **#796 Hardware Preflight first page.** The classic `tab === "connect"` stage now renders the
  approved O1b summary (`workspace/PrepareSummary.tsx`, shared with the workspace's Prepare) in
  `components/PreflightPage.tsx`: one verdict card (headline, detected line, the unchanged
  `prepareVerdict` chip, the Next · Align line, Confirm hardware preflight with the one-line
  refusal note) and "What was checked" as disclosure rows (Camera and acquisition, Focus,
  Fluidics, Calibration, Method, Data destination, All checks). A counted issue opens its row
  and locks it open; on the idle mock camera every row is closed. No configuration on the page:
  "Set up camera… / focus… / pumps…" open the Hardware setup page at that section. The page polls
  only while the classic stage is shown, no workspace is open and no run is active.
- **#797 Left column.** `components/TelemetrySidebar.tsx` replaces the aside's body: a stage chip
  and context line, Camera, Processing, PL core (PZ7035), one Focus section (the duplicate
  "Autofocus" / "Nanopositioner Autofocus" merged), Fluidics from the pumps, Experiment with
  grouped counters; "—" for absent values. The pump status read is one shared poll
  (`components/usePumpStatus.ts`) that `HardwareControls` and the sidebar both consume.
- **#798 Hardware setup page.** `components/HardwareSetupPage.tsx`: header with Back to
  preflight, the Mode notice (its button is the existing Mode switch), then one card per device
  (camera sources and startup selection, Sample and Sheath pump from one `PumpCard`, Focus, Z stage
  as is, pulse generator), dangerous actions in a gated tile whose Stops are always enabled.
  `HardwareControls` and `PulseGeneratorControls` gained an additive `layout` render-prop that
  hands out their parts; one instance keeps the poll, the busy state and the command owner.
  Nothing is deleted: every field and button of the old Connect tab is on this page.

Backend unchanged, no ABI bump. `develop` is untouched until Gavin promotes the branch. The native
webview gate (`desktop/scripts/native-workflow.py`) reaches Configure Mock… through Set up camera…
and returns with Back to preflight. Details and the deviations from the boards:
[[architecture/Desktop-Shell]] ("Classic view summary-first").
