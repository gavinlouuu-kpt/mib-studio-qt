## 2026-10-21 — Workspace click-through polish (#763)

The workspace findings of the 2026-10-21 click-through (`feat/egrabber-workspace` a048f1fb, mock
camera, real backend; report and screenshots on P5 in
`/mnt/hdd/shared/exports/workspace-walk-2026-10-21/`, after-shots in `after-polish/`). Workspace UI
only (`desktop/src/workspace/`); no backend, contract or classic-view change.

- **F6, operator assurance.** A remembered operator is only a suggestion: the plan a page load
  creates names no operator, the remembered ones are offered unselected, and an operator is sent
  (and so recorded with assurance "selected") only once it was typed or picked in this page's Plan.
  The contract has no other assurance value, and none was added. "New plan" keeps the operator
  picked in the plan before it.
- **F8, F11, gate copy.** `camera.session` says "in the classic view, press Start Camera" (the
  backend names the Qt app's Start Live View; the workspace never starts the camera), and
  `processing.background` says "Set Background first" (Background calibration with no background
  finds no empty frame when every frame has cells). `GATE_REMEDIATIONS` / `gateRemediation` in
  `prepareModel.ts`, used by the gate rows and the Start-blocked notice.
- **F9.** Once readiness is confirmed for this setup, the verdict reads "Confirmed · Start blocked:
  …", never "Ready to confirm".
- **F10.** Cards keep their text: `min-width: 0`, wrapping chips and facts, a `fit-content(45%)`
  label column (`workspace.css`).
- **F12.** The single-sample outcome points to Live ("Go to Live"), not the classic view.
- **F13.** "Stop requested · run generation N" goes once that run is terminal or replaced, and
  "Pinned · F…" once its pin's capture ended or it was unpinned.
- **F14.** The Live queue card follows a single sample's run to "Finalized · file closed" (or
  cancelled, or Failed), kept after another run starts (`followSingleRun`, `singleRunChip` in
  `queueModel.ts`). After the Codex review it follows only the experiment operation its start reply
  named (`operation_id`, ended by its terminal OperationStatus event), never the status: the reply
  returns no start generation, and a status on the same file can be another client's run. No
  operation in the reply, or events possibly lost since the Start, reads "Outcome unknown · check
  the file". Binding it to the status would need `start_generation` in the start reply, a contract
  field not added here.
- **Viewer Start.** A tab or client without control cannot start a single sample (the queue already
  could not): "Another tab or client holds control", nothing sent.
- **F15.** When a run start cleared the pins, Live says how many, while that capture is current.
- **F16.** Align says that Write ROI (processing ROI) also starts realtime processing, as the
  classic Apply ROI does. Behaviour unchanged.
- **F21.** One message per Plan issue: a line under the field, a list item in the Start card.
- **F23 (ruling: advisory).** Start shows "Not confirmed in this browser tab" beside it while the
  preflight is not confirmed; Start is not gated on it. The confirmation lives in `sessionStorage`,
  so it is per tab.
- **F22, not changed.** The classic stepper's Experiment stage is "Not started" because
  `deriveExperiment` (`desktop/src/workflow.ts`) needs Camera & Alignment confirmed first, and App
  passes `experimentCompleted = false` on purpose until the backend retains an exact terminal
  outcome (`desktop/src/App.tsx`).

See [[architecture/Desktop-Shell]] ("Opt-in workspace").
