# Desktop Shell (React + Tauri v2)

> The Phase 3 deliverable of epic #246: the React + Tauri v2 desktop app that
> replaces the Qt frontend. It drives the Qt-free C++ backend through the
> [[Rust-Bridge]] (`mib-bridge`, ADR 0003). First vertical slice: **mock camera
> end to end**.

**Source:** `desktop/` — `src/` (React + Vite + TS frontend),
`src-tauri/` (Tauri v2 Rust app), `scripts/xvfb-smoke.sh`
**Bridge:** [[Rust-Bridge]] · **Backend seam:** [[AppBackend]] via
`backend::bridge::BackendFacade`
**CI:** `.github/workflows/desktop-ci.yml`
**Related:** [[../build-and-run/Dependencies]], [[Data-Flow]]

## Layout

The repo root `src/` is the C++ tree, so the whole Tauri app lives under
`desktop/`:

- `desktop/src/` — React frontend. `bridge.ts` is the typed IPC client
  (mirrors the `src-tauri` command DTOs); `App.tsx` + `App.css` are the
  operator shell aligned with the Qt UI on `main` (UI-1, issue #266): a
  File/Settings/Help menu row, a persistent collapsible telemetry sidebar
  (collapse state in `localStorage`), Connect / Overview / Experiment / Review
  tabs with Start/Stop Camera in the tab header, nested Preview / Monitoring
  and App-config / Camera-script tabs, a Review frame + metrics-table split,
  and a metrics status bar with a toggleable log drawer. The backend
  auto-initializes on boot. Every bridged schema-v3 action stays wired
  (Configure Mock…, start/stop capture, live canvas, record, processing
  toggle + px→µm, review load/scrub); controls whose backend surface is not
  bridged yet render disabled with a tooltip naming the blocking issue
  (BE-2…BE-9, #272–#279) — backend/hardware state is never simulated.
- `crates/mib-app-commands/` — the transport-neutral command layer (YOFO
  Studio S5): `AppState` (`Mutex<UniquePtr<BackendBridge>>`), the DTOs, every
  backend command as a plain function over `&AppState`, the event JSON and
  frame-packet encoders, and `dispatch::dispatch(state, host, name, args)` for
  non-Tauri transports (camelCase argument keys, exactly what `invoke` sends;
  `Reply::Json` or `Reply::Binary`). `dispatch::tests` asserts every Tauri
  command except the desktop-only ones is dispatchable.
- `desktop/src-tauri/` — the Tauri v2 app. `src/lib.rs` exposes the shared
  commands as typed one-line `#[tauri::command]` shims and keeps the
  desktop-only pieces (app paths, preferences, updater, installers);
  `main.rs` calls `run()`.
- `desktop/src/review/` + `review.html` — **YOFO Review**, the standalone
  review app built from this tree: the binary without the default `studio`
  feature (`--no-default-features --features review-only`; `main.rs` →
  `src-tauri/src/review_app.rs`, config overlay `tauri.review.conf.json`,
  `npm run tauri:review:build`). It holds only the review bridge; MIB
  Studio's Review tab keeps its own components and commands (ADR 0014,
  decision A). See [[../frontend/YofoReview]].
- `desktop/scripts/xvfb-smoke.sh` — headless GUI smoke launcher.
- `desktop/src/workflow.ts` — pure guided-workflow stage derivation (UX-1),
  with `desktop/src/workflow.test.ts` vitest coverage.
- `desktop/src/preflight.ts` — pure hardware-preflight checklist derivation
  (UX-3), with `desktop/src/preflight.test.ts` vitest coverage.
- `desktop/src/serialReads.ts` — one poll's reads one at a time, newest reply wins
  (`SerialReads`, used by the checklist poll; #736 review), with `serialReads.test.ts`.
- `desktop/src/confirmationSignature.ts` — pure Preflight confirmation
  signature (#713), with `confirmationSignature.test.ts`.
- `desktop/src/workspace/` — the opt-in four-stage workspace on the `@mib/ui`
  kit (P1/P2, #713; P3 Align, #716 A3): `Workspace.tsx` (shell, stages), `PrepareView.tsx` (O1),
  `prepareModel.ts` (pure tones and summaries), `usePrepareReadiness.ts`
  (path-less readiness poll), `AlignView.tsx` (O2), `alignImageModel.ts` (view transform,
  probe, histogram, profile, `FrameInspector`), `quickControlModel.ts` + `useQuickControl.ts`
  (requested → readback machine), `alignControls.ts` (which controls an instrument offers);
  O3 Experiment · Plan (#716 A5): `ExperimentView.tsx` (sub-views Plan / Live / Summary),
  `PlanView.tsx`, `planModel.ts` (pure: sheet, identity, `buildStartRequest`, bounds, refusals)
  and `usePlan.ts` (the plan held by App, remembered operators); O4 Experiment · Live (#716 A6b):
  `LiveView.tsx`, `liveModel.ts` (pure: names, ended pins, pin outcomes, scatter and its click
  mapping, status lifecycle, the stops), `useLiveCells.ts` (the cell store's reads, crops and pin
  commands) and `liveImages.ts` (a crop or mask packet as an image URL).
  See "Opt-in workspace" below.
- `desktop/src/experimentCounters.ts` — operator saved counts by class, pending
  saves derived with exact u64 arithmetic, policy drops and writer failures
  from the existing experiment status fields (#546); Vitest covers labels.
- `desktop/src/quality.ts` — pure Camera & Alignment quality-gate derivation
  (UX-4), with `desktop/src/quality.test.ts` vitest coverage.
- `desktop/src/contextBar.ts` — pure persistent active-context bar derivation
  (UX-8), with `desktop/src/contextBar.test.ts` vitest coverage.
- `desktop/src/commissioning.ts` — pure Operator/Service-mode actuation gate
  (UX-9), with `desktop/src/commissioning.test.ts` vitest coverage.

**Guided workflow (UX-1, issue #305):** `desktop/src/workflow.ts` layers an
authoritative *stage state* on the Connect / Overview / Experiment / Review
tabs. `deriveWorkflow(facts)` is a pure function of backend snapshots (camera
selection/running, processing-core pin, experiment status, review metadata)
plus explicit operator confirmations, returning each stage's status
(`not-started` / `needs-attention` / `ready` / `running` / `complete`), its
blocking checks, and the single recommended next action. Preflight and
Alignment reach `complete` only via an explicit confirmation whose stored
*signature* still matches — detection alone never completes a
stage, and changing the device invalidates the confirmation. Since #713 the
Preflight signature (`confirmationSignature.ts`, JSON) binds the camera (mode,
label), the host core version, the PZ7035 PL build and weights ids, the pump
identities (model, port, Modbus address), the coarse Z stage and nanopositioner
identities, the applied profile id and revision, and the recording destination
the backend reports (PZ7035 `instrument.storage.path`; none on the desktop before
Start). A value not read yet is `null`, distinct from a device read as absent, and
only identities are bound (never flow rate or position), so polling does not
invalidate a confirmation. The Alignment signature is still the camera identity. The shell renders
status on each tab (text + dot, never colour alone) and a "Next" action banner,
and lands startup on the earliest incomplete stage. Unit-tested in
`workflow.test.ts` (`npm test`, gated in Desktop CI). Detailed per-stage content
is the rest of epic #304 (UX-2…UX-11). Details:
`knowledge_map/task/2026-07-21-ux1-guided-workflow.md`.

**Hardware preflight (UX-3, issue #307):** `desktop/src/preflight.ts` —
`derivePreflight(input, requirements)` builds the Preflight stage's checklist
(camera, processing-core/trust, capture stream, autofocus, sample/sheath pumps,
trigger, storage), each a `passed`/`warning`/`failed`/`unknown`/`unsupported`/`not-required`
status with requirement, expected-vs-detected identity, cause, recovery actions and
`blocks`; `criticalPassed` is true when no check blocks. Since #713: a value not read
yet (backend not initialized, core status, PL core / sensor link / LED before the first
instrument poll or while the link rates are still measured, a device snapshot with
`valid: false`) is `unknown`, never `failed` or `not-required`; a PL build or weights that
cannot be verified (`build_match` / `profile_match` "unknown": no
`/etc/yofo/expected-core.json`, no pinned weights) is `unknown` too, with the same fix-it
text; a capability the instrument lacks (`capabilities.autofocus`/`trigger` false) is
`unsupported`; storage nothing reported is `unknown` (the desktop checks the file at Start).
`checkGateStatus` maps the states to the backend's Pass / Warn / Fail / Unavailable /
NotRequired (unknown → Unavailable, never Pass). Blocking is unchanged from #548, failing
closed: a required check that failed, is unknown or warns blocks; unsupported and
not-required rows never block. Backend gates never unblock a row (the `science.pl` gate,
for one, never checks the PL build identity); relaxing a specific warning would need an
audited per-warning acknowledgement (#310). Required/optional/n-a per device
is meant to come from the selected profile (not bridged yet → `DEFAULT_REQUIREMENTS`
until UX-2 #306). The shell polls camera/core/autofocus/pumps/trigger every 1.5 s
(`PREFLIGHT_POLL_MS`) while the Preflight tab (or the workspace's Prepare stage, or
Experiment's Plan) is shown, off the capture loop, plus the Z stage status on the desktop for
the signature, and renders the checklist in the Connect tab. One read at a time
(`serialReads.ts`, #736 review): a tick or Refresh while a read is in flight is skipped, a read
stuck for 10 s (`PREFLIGHT_READ_STALE_MS`) stops holding the poll, and a reply older than one
already applied is dropped, so stalled reads do not pile up and a late reply never overwrites
newer state. Storage reads `unknown` until a backend reports the destination
(the PZ7035 does). Details:
`knowledge_map/task/2026-07-21-ux3-hardware-preflight.md`.

**Required checks gate the workflow (#548).** `WorkflowFacts.requiredFailures` carries the
checklist's blocking checks (`blocks`, "Label: detail"). When present it is the
authority for the Preflight stage and replaces the host core pin test, because the checklist
already holds the host core check on the desktop and the PL core check on the PZ7035. Each such
check is a blocking reason, so the stage is "needs attention", the recommended action navigates
instead of offering the confirmation, and a stage confirmed earlier goes back to "needs
attention" if one starts failing. A required check in `warning` blocks as well, and since #713
so does a required `unknown` (formerly rendered `failed`). The three #548 cases stay pinned as
blocking in `preflightPz7035.test.ts`: an unverified PL build, sustained sensor-link errors, the
LED on a service setting. Tests: `workflow.test.ts` (#548), `preflight.test.ts` and
`preflightPz7035.test.ts` (#713).

## Opt-in workspace (P1/P2, #713)

`desktop/src/workspace/Workspace.tsx` is the four-stage Prepare / Align / Experiment /
Review shell on the `@mib/ui` kit (`AppShell`, `StageTabs`, `Toolbar`, `Card`, `FactList`,
`StateChip`, `Button`, `Notice`, `DataTable`, `OptionGroup`, `ConditionSwatch`, `Stepper`, `CellImage`,
`CellStrip`, `AreaDeformChart`, `StatTile`), the Operation app's steps A1, A3, A4b, A5 and A6b (#716). The kit stylesheets are imported once, there; `workspace.css`
only places cards with the kit's spacing tokens (and styles the app-local number and text fields
of the quick controls and the sample sheet, until the kit has them).

- **Opt-in, session only.** The menu row's "Workspace preview" button opens it; "Classic
  view" closes it. The flag is plain React state: every start and every reload opens the
  classic view, so startup is unchanged. While the workspace is shown, the classic menu row,
  body and context bar are hidden with `display: none`, not unmounted, so nothing in the
  classic tree mounts, unmounts or changes tab because of it; the status bar and log stay.
- **Navigation never actuates.** The workspace never changes the classic `tab`: a classic tab
  change can act (Camera & Alignment switches the camera to the full sensor, and the PZ7035 to
  Align with its LED; Experiment to Run). Stages only change what is shown. In Align the
  same switch is a button the operator presses ("Show the full sensor" / "Switch to Align").
  Experiment opens on its Plan, where a run starts only on "Start run" (or a sample queue's
  Start), stops only on the queue's "Next sample" (or Live's "Stop & finalize run"), and the camera
  goes to the mode a run records in only on the Plan's "Switch to Run" (PZ7035) or "Show the
  experiment window" (desktop). Its Live sub-view reads and turns monitoring on while shown (the
  classic Monitoring tab's visibility gate), and pins, stops or stops the camera only on a button.
  Its Summary sub-view, and the Review stage, say "Not yet in the workspace" and offer "Back to the
  classic view".
  Prepare's "Camera / Focus / Pump setup" leave for the classic Connect tab, which only reads.
  The classic startup landing is skipped when the workspace is already open.
- **Prepare (O1)** shows the checklist by subsystem (camera and acquisition, focus, fluidics,
  data destination) and the authoritative backend gates read **without a path**:
  `bridge.fetchExperimentReadiness("")` (`PREPARE_OUTPUT_PATH`), every 2 s
  (`PREPARE_READINESS_POLL_MS`) while Prepare is shown and no run is active, one request at a
  time. The backend then reports `storage.output` Unavailable ("no output path chosen yet") and
  creates no probe file (`ExperimentCoordinator`, the `outputPath.empty()` branch); Start
  re-evaluates every gate with the chosen file. A failed read clears the gates (never stale
  gates as current). Gate rows show a plain label (`GATE_LABELS`, one table for every backend
  gate id; an unmapped id is shown as itself) with the raw id as secondary text and title, the
  state and reason as a chip, and the remediation ("Fix: …"). Tones: Pass → confirmed,
  Warn → warn, Fail → blocked, Unavailable → unknown, NotRequired → neutral. Two remediations are
  the workspace's own words (`GATE_REMEDIATIONS`, `gateRemediation`; #763): `camera.session`
  ("in the classic view, press Start Camera …"; the backend names the Qt app's Start Live View, and
  the workspace never starts the camera) and `processing.background` ("Set Background first …":
  Background calibration with no background set finds no empty frame when every frame has cells).
  Chips, facts and "Fix:" lines wrap inside their card (`min-width: 0`, a `fit-content(45%)` label
  column; #763 F10).
- **Calibration card (Prepare; #716 A4b, bridge ABI 42; the method report since ABI 43).**
  `CalibrationCard.tsx`, its pure model
  `calibrationModel.ts` and the hook `useCalibration.ts` (held by `Workspace`, so Prepare and the
  Plan share one read). See [[../services/CalibrationRecords]].
  - **Reads.** `calibration_records_list`, polled with the readiness (every 2 s, while Prepare or
    the Plan is shown and no run is active, one request at a time; a read asked for meanwhile runs
    once it ends). A failed read clears the list, so it reads "Unknown · <error>", never "None";
    so does a store the backend cannot read (`store.error`: "Unknown · the calibration store cannot
    be read", and the Plan declares nothing). A read started before an apply answered, or in an
    earlier active period (before a run, another view), is dropped. `calibration_record_get` only
    when the operator selects a record in the list (its versions); only the newest get's reply is
    shown.
  - **What it shows.** The `calibration.record` gate (label "Calibration record") with its state
    and the backend's `Fix:`; the record in effect (the backend's `in_effect_record_id`, never
    recomputed) with its id, factor, age since it was applied, method and revision, identity
    (camera · ROI · chip · objective) and source file; the current factor and setup; an applied
    record that is no longer in effect, with why (its factor is not the current one, e.g. after a
    restart, or the setup changed); every proposed record with current → proposed, its evidence
    and whether it is applicable here (a proposal of another setup, or with the identity unknown
    or no factor reported, is shown with the reason and its button disabled; a record whose stored
    evidence fails the backend's re-check, `evidenceFails`, gets no button at all); the newest
    insufficient records with their failed gates in plain words ("Valid objects: 120, at least
    200"), never with an Apply; and a plain `DataTable` of every record, newest first (at most 20
    rows), no trend chart. A null stays "Not recorded", never 0.
  - **Apply / Re-apply** (`calibration_apply(record_id, expected_current_factor)`) is the card's
    only action, on its button, with the `current_factor` of the list read the page shows. After
    a restart a `required` method's gate fails with "re-apply record <id>"; the card offers
    "Re-apply <id>" directly beside that Fix (`fixAction`; a proposal of this setup otherwise), as
    the PR #771 ruling asks. The reply's previous → new is shown ("Applied · cal-… · Pixel-to-micron
    0.5 → 0.52 µm/px"). Every refusal is said in plain words and changes nothing on screen but the
    re-read (`applyReplyNotice`: stale factor, a run active, insufficient, another setup, an unknown
    identity, not applicable, store failure; `applyErrorNotice`: the server's VIEWER_ONLY, a lost
    reply). Whatever the answer the list and the gates are read again at once; a list read that
    started before the apply answered is dropped, as is a gates read in flight (an explicit refresh
    in `usePrepareReadiness` reads once more), and the buttons stay locked until the list read
    after the answer settles (at most `RECONCILE_MS`, 5 s), so a second press never acts on the list
    from before the apply. A viewer's buttons are disabled with the shared
    control block (`controlBlock("Apply")` in `planParts.tsx`, the words of the Plan's
    `CONTROL_BLOCK`) and nothing is sent; during a run nothing is read or sent. Navigation, listing,
    selecting and polling never send `calibration_apply` (`CalibrationCard.test.tsx` and
    `navigation.test.tsx` count every IPC call).
  - **The applied method (bridge ABI 43, #776; `methodCalibrationOf`).** The card reads the
    readiness report's `method_calibration` from the readiness read that already happens (no extra
    IPC), exactly as the `calibration.record` gate read it. Purpose and policy are never inferred
    from the gate's text. Rows under the gate:
    - **Applied method**: for `purpose: "calibration"`, "This method is a calibration method:
      closing its run derives a record" (`CALIBRATION_METHOD_TEXT`); for `declared: false`,
      "Measurement method · policy none: it declares no calibration requirements"; otherwise
      "Measurement method".
    - **Reference** (calibration methods only, `referenceText`): "7 ± 0.5 µm diameter · CV at most
      10 % · nominal E 3 kPa", the nominal E only when the method declares one.
    - **Policy** (`policyText`, `policyDetail`): the report's `calibration_policy` and
      `calibration_max_age_hours` ("required · max age 24 h"), with what that means for the gate
      (a calibration method itself needs no record).
    - When `error` is set (the declarations cannot be read; the gate fails closed), the error is
      shown in the gate row beside its Fix, the method reads "Unknown · its calibration
      declarations cannot be read", and the default purpose / policy the report carries are not
      shown as the method's.
    - When the member is `null` (a backend before ABI 43, or the report not read or `valid: false`),
      none of these rows appear and the A4b wording stays, with the purpose-gap note.
  - **Running a calibration** is a normal run: the card says to apply the calibration method in the
    classic view (or, with a calibration method applied, that it is applied:
    `RUN_APPLIED_CALIBRATION_TEXT`) and record the beads from Experiment · Plan with Start run, and
    what the backend derives when that run closes (`DERIVATION_TEXT`). There is no separate
    acquisition path. Only when the method report is `null` does the card add the purpose-gap note
    (`PURPOSE_GAP_TEXT`: the workspace cannot tell whether the applied method is a calibration
    method; the gate row shows the backend's own words).
- **One verdict** (`prepareVerdict`), the same words in the header chip and the "Ready to
  operate?" card, combining the checklist and the gates. It never contradicts the Confirm
  button, which the checklist alone drives:
  - **Not ready**: a required check blocks. The counts and plain names follow ("Not ready ·
    3 unknown, 1 warning").
  - **Readiness paused · a run is active**: the gates are not read and Confirm is locked.
  - **Ready to confirm · Start blocked: <labels>** (warn tone): the checklist passes, but a gate
    other than the pending destination is Fail or Unavailable. Start refuses both. With the
    gates not read: "Ready to confirm · gates not read yet" (unknown tone). Once the operator
    confirmed readiness for this setup, the same states read **Confirmed · Start blocked: …** /
    "Confirmed · gates not read yet" (#763 F9).
  - **Ready with warnings**: nothing blocks, but a check or gate warns.
  - **Ready**.
  
  The destination gate is never counted: "Ready · destination checked at Start".
- **Confirmation.** "Confirm readiness" (the one primary action) stores the current Preflight
  signature, the same state the classic workflow bar sets. It sends nothing to the backend.
  - It is disabled while a required check blocks, with no camera, or during a run.
  - It is not gated on the backend gates, as in the classic Preflight. Some gates can only pass
    in a later stage: `processing.roi` needs live view (Align), and the destination comes at
    Start.
  - When disabled, a Notice next to it says why in plain words ("3 required checks not read
    yet or not verified: PL core (build + weights), Sensor link, LED strobe";
    `confirmBlockers`).
  - When it is available but a gate refuses Start, a warn Notice says so ("Start will be
    refused until: Processing core — activate the pinned core"; `startRefusalText`).
  - It is advisory (#763 F23 ruling): it is kept per browser tab (`sessionStorage`, it survives a
    reload of the tab) and Start does not require it. Without it, Plan's Start card and the Queue
    card show "Not confirmed in this browser tab" beside Start, with that note (`startConfirmation`).
- **Align (O2, P3; #716 A3).** Penpot board O2 (`7832aa3b-faed-4e2c-ae5c-0b72f1e4fecb`, read at
  revn 218). Two cards: the live image and the quick controls.
  - **The image is the classic preview's stream.** App's `live` frame view hands every packet it
    already pulled (`fetch_frame_packet`, at the display rate) to one sink after the classic
    draw (`subscribeAlignFrames`); there is no second capture path, and the sink only keeps a
    reference. `FrameInspector` computes the histogram (at most 65,536 pixels on a regular grid)
    and the line profile (at most 1,024 points) from the newest frame at most every 250 ms, on a
    timer, never in the delivery callback; frames in between are skipped, not queued. Painting
    is one `requestAnimationFrame` per delivery at most. A frame only the workspace shows counts
    in the status bar's display rate (`countDisplayed`).
  - **Image tools:** fit, 1:1, zoom (+ / −, wheel, keys) about the cursor, pan by drag or arrow
    keys (`alignImageModel.ts`: one transform, clamped so the image never leaves the view), a
    pixel probe (coordinates and the raw Mono8 value through the row stride), a click pins the
    point the row or column profile runs through, the ROI outlined when the frame is the area it
    is placed on, and "Freeze display", which holds the shown frame while capture and polling go
    on. A frozen display keeps its frame's size too (the inspector refuses the packet, and only an
    accepted packet changes the view's image size), so a camera that starts sending other
    dimensions meanwhile (e.g. after "Show the full sensor") cannot skew fit, zoom or the probe.
  - **Quick controls (requested → readback).** Only what the classic tab can write, through its
    commands: the camera ROI (`save_camera_roi` via `useCameraWindow.save`, readback the
    geometry read after the save), the PZ7035 Run window (placed, applied by the switch to Run;
    readback `restoredRunWindow` of `fetch_instrument_status.mode`) or the processing ROI
    (`set_processing_roi`, the classic Apply ROI's `applyProcessingRoi`; readback the applied
    config's `roi`). Writing the processing ROI applies the processing settings, as Apply ROI does,
    so it also starts realtime processing when the settings enable it, and the live cells begin a
    new capture; the card says so (`PROCESSING_ROI_WRITE`, #763 F16). Exposure and frame rate are read-only readbacks of
    `fetch_camera_geometry.session` (Aravis; "Unavailable · not reported" otherwise), the
    PZ7035 Run rate and exposure read-only from `fetch_run_fps` (set in the camera document),
    gain "Unavailable · not bridged". `alignControls.ts` derives the rows from the capabilities.
    `quickControlModel.ts` is the machine: bounds and increments are checked before sending
    (off-step values snapped with a note, out-of-range refused with the range); a write is
    pending until its own reply, whose readback is read after the write; a readback seen
    meanwhile never settles it; a refusal shows the reason and restores the readback; no reply
    in 5 s (`WRITE_TIMEOUT_MS`) shows "Timed out" and the readback, and the late reply is
    ignored (stale token). Geometry and processing-config reads that answer out of order are
    dropped (`geometryReads`, `configReads` in App), so a readback from before a write never
    replaces one from after (`alignReadback.test.tsx`: a late config readback of a timed-out
    write after a newer write completed).
    Locked while a run is active. All features, the XML and scripts stay in the classic view.
  - **The mode switch is a button.** `alignMode` offers the classic tab's entry switch when the
    image is not the full sensor (desktop camera with geometry: "Show the full sensor",
    `set_camera_overview(true)`; PZ7035 outside Align: "Switch to Align", `set_instrument_mode`),
    disabled during a run or while a stopped Run holds its frames. App's `applyCameraMode` is
    the one function behind the classic tab change and this button.
  - **Leaving the workspace never acts.** While the workspace is shown the hidden classic tab
    switches nothing (its mode effect returns early, so a Run window placed in Align does not
    send the hidden Experiment tab to Run). If the workspace switched the mode, or a classic
    switch was skipped meanwhile, the classic Camera & Alignment or Experiment tab shows a kit
    `Notice` (`components/ClassicModeNotice`, `classicModeNotice` in `cameraWindow.ts`) with
    the mode the camera is in, the one the tab uses, and an explicit button ("Switch to Run",
    "Show the experiment window", …) that calls the same `applyCameraMode`. The next classic
    tab change switches as it always did.
  - **Opening the workspace cancels a pending classic switch.** A classic tab change (or the
    notice's button) first awaits a geometry read; "Workspace preview" sets the stage ref at
    once, and `applyCameraMode` rechecks "cancelled or workspace shown" after every await and
    again right before `set_camera_overview` / `set_instrument_mode`. A switch cancelled this
    way sends nothing and leaves the classic tab showing the notice's mismatch, not a silently
    wrong mode.
  - While Align is shown, App polls `fetch_camera_geometry` every 2 s, as the classic tab does.
- **Experiment · Plan (O3, P4; #716 A5).** Penpot board O3 (`14db68fb-c9d2-4e21-ae5a-bd4291fc6273`,
  read at revn 218; per-run auto-stop, the queue's duration/size preview and "Base on …" are later
  steps), with O4's queue strip and run lifecycle (`22f62c97-3b47-43f4-bb9c-96eb2be8c257`) and O5's
  "Next sample" (`eb7b196d-6e3e-47c2-bb65-183b19329704`), both read at revn 218. Three cards and a
  Start card (a Queue card for a sample queue); the plan is local until a button acts.
  - **The plan lives in App** (`usePlan`), so it survives stage changes and leaving the workspace;
    a reload starts a new one. It holds the format, the sheet rows and the selected operator.
    Creating or editing it sends nothing.
  - **Sample format** (`OptionGroup`): "Single sample" (one run, one file) and "Sample queue"
    (one run and one file per sample, below). With more than one sample in the sheet "Single
    sample" is inert with the reason ("The sheet has 3 samples · remove all but one first"); the
    format is fixed once a run started.
  - **Sample sheet**, the fixed schema of `/experiment_annotation` ([[../data-model/HDF5-Storage]]):
    sample, condition, experimental unit, concentration (value + unit), channel, medium,
    temperature (°C), notes and free key/values (strings, unique keys). A compact `DataTable`
    shows the row as it will be recorded ("–" = not declared), the condition with its
    `ConditionSwatch` (`conditionSlots`: distinct conditions take slots 1–4 in sheet order;
    slot 5 is the kit's calibration colour); the editor below it has one app-local field per
    member. No `maxLength`: a long value stays as typed and is refused.
  - **Client checks mirror the contract** (`planModel.ts`, the generated
    `EXPERIMENT_START_REQUEST`, bytes of UTF-8 as the backend counts them): every text bound,
    control characters (notes may hold tab, CR, LF), concentration value and unit both or neither
    (a finite number ≥ 0), temperature from `temperature_c_min` to `temperature_c_max`, at most
    `max_free_entries` free entries, a half-filled or repeated key/value row, and
    `max_request_bytes` of the text as sent. Each refusal names the field and the limit next to
    the field and in the Start card ("Condition: too long: 257 bytes, at most 256. Shorten it;
    nothing is cut off."), one message per issue: a line of its own under the field, a list item in
    the Start card (#763 F21); nothing is truncated. Spaces around a value are not part of it; an
    empty field is not declared and its key is left out, never sent as `""` or `null`. The
    backend checks the same request again and stays the authority.
  - **Identity** (`buildStartRequest`, one ordered serializer so the text is exact):
    `experiment_id` is a UUID v4 minted with `crypto.getRandomValues` when the plan is created
    (lowercase; `randomUUID` is missing outside a secure context, as on the instrument's HTTP
    UI), `run_index` 1, `sample_ref` the row's ref ("S1"), `operator {id, name}` when one is
    given, `calibration_record_id` (below), then `annotation` when anything is declared.
    The operator is the one the operator selected or typed (#393 phase 1), recorded by the
    backend with assurance "selected"; the card says it is not authentication. Operators are
    remembered on this machine (`localStorage` `mib.workspace.operators`, most recent first, at
    most 8) when a run starts with them. A remembered operator is only a suggestion (#763 F6): the
    plan a page load creates names no operator, the remembered ones are offered unselected, and an
    operator is sent only once it was typed or picked in this page's Plan (the contract has no
    other assurance value, so nothing else is recorded as "selected").
  - **The calibration record a Start declares (#716 A4b).** `buildStartRequest(plan, row, run,
    calibrationRecordId)` declares `calibration_record_id` only as the record the page showed as in
    effect (`declaredCalibration`: `in_effect_record_id` of the last successful list read); with
    none in effect, or the list not read, nothing is declared and the backend records the record in
    effect itself. The Run identity card (single and queue) shows it ("cal-… · in effect (0.52 µm/px)
    · declared at Start"; "None in effect when last read · not declared; the backend records the record in effect at Start"; "Unknown · … · not declared") and the
    calibration gate in the backend's words. Since ABI 42 the id is a check: a record applied,
    re-applied or taken out of effect between that read and Start makes the backend refuse the
    Start (`InvalidRequest`, key `calibration_record_id`, "(mismatch)"); the Plan says "Not started ·
    the applied calibration changed since this page read it" beside the calibration record
    (`calibrationChanged`, `calibrationChangedText`; the queue's step says the same), keeps the plan
    and its identity (a refusal records nothing), and reads the list again. The queue's Start
    declares what is shown at each press (`startCurrent(calibrationRecordId)`).
  - **Start run** (the one primary action) runs App's `runExperimentStart`, the classic Start
    flow (`experimentStart.ts`: ask before a stopped Run's frames are discarded, pick the file,
    readiness with it, start) with the plan's `startRequestJson`; the classic Start button runs
    the same function without one and sends none. It is disabled, each reason a `Notice` next to
    it (`startBlocks`), while Prepare's checklist blocks (the same `prepareVerdict` title and
    `confirmBlockers` lines as Prepare; the workspace reads the checklist and the path-less gates
    while the Plan is shown, so the verdict is the same; `workspaceReadsPrepare(stage, view)` is
    the one predicate for both polls, and the Live and Summary placeholders read neither, so App
    holds the sub-view), for the classic Start button's reasons
    (`startExperimentReason`: backend, camera stream or Run mode, a run active, a start pending),
    while the sheet has an issue, while a start is pending, and once the plan's run has started.
  - **The start reply alone decides the outcome** (#736 review). `startExperiment` returns
    `started`, `refused` or `unknown` from the reply, `unknown` also when the start call itself
    threw (the request was sent, its reply was lost), and throws only for failures before sending
    (dialog, readiness). Since ABI 40 (#741) the reply's `start_outcome` decides, not its message
    (`startReplyState`, `START_OUTCOME_STATES` in `experimentStart.ts`): Started → started;
    StaleReadiness, AlreadyActive, Busy and InvalidRequest → refused (the coordinator returns them
    before it begins the run: no start generation, no file); NotReady, StorageFailed and
    ProvenanceFailed → `unknown` with the `code` (the coordinator can return them after it opened
    the run's HDF5 file: `initializeDatasets` failing closes the file without removing it, a
    snapshot or annotation write failing removes it best effort, and on the PZ7035 "execution
    provider did not start" comes after the file and snapshot were written, under NotReady, which
    the code cannot tell apart from the readiness refusal before anything). A code that contradicts
    `ok`, or one the table does not know, is `unknown`. Without a code (a backend before ABI 40, or
    a code the decoder dropped as unknown) `ok` decides, as before. App's status refresh after the reply is a separate read: its failure is
    `refreshError`, shown as its own warn Notice ("Run status not refreshed"), never as the
    outcome. The plan locks on `started` and on `unknown` (the run may exist), so a started run's
    `experiment_id`/`run_index` are never sent again, whatever a later read did.
  - **Outcomes.** The backend's `InvalidRequest` (start outcome 7, decided by the code; the
    `invalid start request: ` prefix decides only for a reply without a code; the message is
    "invalid start request: <key>: <why>", `invalidRequestReason(message, code)`) is shown in the sheet
    card in plain words ("Condition: 300 bytes, at most 256. No file was created. The plan is
    kept"), with the key as secondary text; the plan, its id and the values are kept and nothing
    claims a run started. Not ready, another refusal or an error before sending are said as they
    are and keep the plan; a cancelled dialog says nothing; a lost reply says "Start outcome not
    known" and locks the plan; a locking code says "Not started · its file may exist" with the
    backend's message and locks the plan too (its identity may be in a file left behind). A started run locks the plan ("Run 1
    started · <path>"): its identity is in that run, so a single-sample plan has no second run;
    "New plan" mints a new id and keeps the operator (typed or picked in this page's Plan) and the
    format. The outcome points to Live, where the run is followed and stopped, with "Go to Live"
    (#763 F12).
  - **A viewer's Start.** In a tab or client that does not hold control, the single-sample Start is
    disabled, as the queue's is, with "Another tab or client holds control" (`CONTROL_BLOCK` in
    `planParts.tsx`); nothing is sent.
  - **The camera mode a run records in (A5, #739 review).** `planCameraMode` (`planModel.ts`) and
    the Start (or Queue) card's kit `Notice` "Camera · <mode>" offer the classic Experiment tab's
    entry switch as a button: App's `switchPlanCameraMode` calls the shared
    `applyCameraMode("experiment")` and marks the classic tab's mode stale for its
    `ClassicModeNotice`. Navigation never switches.
    - PZ7035, while the instrument is not in Run: "Switch to Run", disabled with the reason during a
      run, with no Run window placed ("place the 512×96 Run window in Align first (Write Run
      window)") or while a stopped Run holds its frames; a refused switch shows the backend's reason.
      Start stays disabled with the classic reason ("The camera is not in Run mode yet").
    - Desktop camera left on the full sensor (Align's "Show the full sensor"; the geometry's
      `overview`): "Show the experiment window", and Start (single and queue) is disabled with "The
      camera shows the full sensor, not the experiment window" (`startBlocks`' `camera` block), since a
      run would record the full sensor. The classic Start is unchanged: the classic Experiment tab
      switches on entry.
- **Sample queue (O3/O4, P4; #716 A5, ADR 0015 items 1–2).** A guided flow over the existing
  commands, with no backend queue state and no new bridge command; since ABI 40 (#741) it reads the
  start and stop outcome codes and names its own run on `experiment_stop`.
  `queueModel.ts` is the pure state machine; `usePlan` holds it (App, so it survives stage changes
  and leaving the workspace) and is its controller; `QueuePlan.tsx` the view; `planParts.tsx` the
  parts both formats share.
  - **The sheet** holds N rows under the plan's one `experiment_id`, each with its own ref
    (`sample_ref` S1, S2, …; `nextSampleRef` never gives a removed ref out again) and the same fixed
    schema; one colour per condition. A selectable `DataTable` shows each row's run index (planned in
    muted text until its Start, "1?" for a lost reply), state chip, issue count and output file (the
    backend's `output_path`, the name with the path as tooltip); the editor edits the selected row.
    "+ Sample" adds a row at any time; ↑ / ↓ / Remove only for rows not yet started (the started rows
    are a fixed prefix). `editKeepsRecorded` refuses any edit that would change a row with a started
    or unknown attempt, its order, the format, the operator or the experiment id once the queue began.
  - **The flow**, each step one button and at most one command: **Start S1** (App's
    `runExperimentStart`, the classic Start flow: pick the file, readiness with it, `experiment_start`
    with `buildStartRequest(draft, row, nextRunIndex)`) → running → **Next sample** ("Stop last
    sample" for the last row; App's `stopForPlan`: `experiment_stop` with `expectedStartGeneration`,
    the row's own `start_generation` as seen in the status) → the drain and
    finalize are followed in the experiment status App already polls every 500 ms (Stopping/flushing,
    then terminal), with the counters and committed/admitted shown → **Confirm closed** (sends
    nothing; the backend closed the file, the operator confirms) → "Swap the tube for S2 · …" →
    **Start S2** (readiness with S2's file at its Start, as always). Nothing advances, stops or starts
    on its own: a run that ends without Next sample (the classic Stop, the server's safe stop) is
    followed to Finalized and still waits for the close confirmation and the next Start.
  - **A run is the row's only on the row's own file (#739 review).** A Start records the
    `start_generation` of the last status read (Start is unavailable until one was read). A status
    counts for the row only when its `output_path` is the row's chosen file (`recordedPath`: the
    backend appends ".h5" unless the path ends in ".h5" or ".hdf5") and its generation is newer than
    that read; an older read answering late is ignored. A newer generation on another file, or a
    later generation after the row's run was seen, means another run took over the instrument: the
    row is **interrupted** ("another run started on this instrument", unknown tone), its run's end is
    not known, and the queue never stops or confirms that run (Retry as a new run, Skip or End
    queue). "Next sample" is enabled only while the status shows the row's own run active
    (`ownRunActive`: its generation, its file, Active), because `experiment_stop` stops whatever run
    is active; until the first such read the step says "waiting for the status to report its run".
    **Next sample stops only the row's run (ABI 40, #741).** `usePlan.stopCurrent` passes the row's
    `start_generation` (the generation `ownRunActive` matched, a canonical decimal string) and App's
    `stopForPlan` sends it; the backend compares it under the lock that accepts the stop. A
    `stop_outcome` RunMismatch (3: another run is active, or none is) stopped nothing: the row is
    **interrupted** at the stop ("<ref> · not stopped · run N is not the backend's active run", chip
    "Interrupted · not stopped", the backend's reason in the text), and the queue sends nothing more
    for it, with or without the generation. A later status of another run keeps that reason; a
    status of the row's own run Stopping or terminal (its generation, its file) is still followed, so
    a run that had ended by itself reaches Finalized and its close. Busy and NotActive keep the
    "stop was not accepted" handling, as does a reply without a code. The classic Stop Experiment is
    not tied to a row and sends no generation; the single-sample plan has no stop of its own (it is
    stopped in the classic view). Phases only move forward, except that a row interrupted at its stop
    can still reach its own run's end.
  - **Identity (#736 rules).** Each row's start reply alone decides started / refused / unknown.
    Every Start sends `nextRunIndex`, which grows on started and unknown only, so a run index is
    never sent twice; a refused Start recorded nothing and keeps its index. Started and unknown rows
    are locked and stay visible.
  - **Failures stop the flow at their step** with the reason (kit tones) and the operator's choice:
    a refused Start (the InvalidRequest named in plain words, not ready, an error before sending, a
    pending start) → correct and Start again, Skip or End queue; a lost start reply (unknown) →
    "Retry S1 as run 2", Skip or End queue; a start answered with a locking code (NotReady,
    StorageFailed, ProvenanceFailed) → "Not started · S1, run 1 · its file may exist", unknown and
    locked like a lost reply (chip "Not started · file may exist"), "check whether the backend left
    the run's file", Retry as run 2, Skip or End queue; a stop refused as RunMismatch → interrupted,
    nothing more sent, Retry as a new run, Skip or End queue; a stop that is not accepted or gets no reply → the run is
    shown running with the reason, Next sample sends it again, End queue says the run keeps running;
    a failed run (Failed, finalization not ok, or a fault) → the reason and the kept file, Retry as a
    new run, Skip or End queue. End queue is refused while a Start is pending, a run runs (unless its
    stop failed) or a finalized run waits for its close confirmation.
  - **Lifecycle stepper** (kit `Stepper`, `lifecycle`) for the current row: Planned → Starting →
    Running → Stopping · drain → Finalized → Closed; a refused or failed step is `failed` and an
    unknown Start `current`, each with a kit `StateChip` (the Stepper has no unknown state, #715).
    The Queue card sits under the sheet while planning and above it once the queue began, so the
    step to press is in view during a run.
  - **A control loss withdraws a pending Start (#739 review).** `usePlan` bumps an epoch on every
    control loss; each Start (single or queue) passes `stillWanted` to the Start flow
    (`startExperiment`), which asks it after the discard question, after the file dialog (whose
    answer is then not used), after readiness and immediately before `experiment_start`. A withdrawn
    Start sends nothing (outcome `withdrawn`: "Not started · control was lost while the Start was
    pending"); the operator presses Start again after Resume.
  - **A lost controlling client pauses the queue** (`useHoldsControl` in
    `transport/useControlRole.ts`: always held on the desktop; in a browser only while connected and
    named the controller). Paused, every queue button is disabled and the machine refuses every
    action; status facts still apply (the server's #711 safe stop may finalize the run meanwhile).
    It never resumes on its own: "Resume queue" needs control again and sends nothing, and the
    operator then presses the step (e.g. Confirm closed) again. Without control, Start says why.
  - Not in this step: per-run auto-stop (a backend stop condition with its own ABI number),
    "base on previous experiment", the queue's duration/size preview, Summary and Quick look. Live is
    the next bullet.
    A backend restart that remounts the browser app (a new boot id) starts a new plan: the queue is
    local by design, the recorded files stay.
- **Experiment · Live (O4, P4; #716 A6b).** `LiveView.tsx` on the reviewed board O4, over the live
  cell store of ABI 41 (A6a, [[../services/MonitoringCellStore]]) and today's commands; no new bridge
  command. `liveModel.ts` holds the pure rules, `useLiveCells.ts` the cells' reads, crops and pin
  commands, and `liveImages.ts` draws a packet for the kit.
  - **Layout, as the board.** The toolbar: Plan · Live · Summary, then **Stop acquisition**,
    **Freeze display** and **Stop & finalize run**. Then Live cells (the live frame and the strip)
    beside Area–Deformability; Pinned cell beside Recording and run status; Sample queue beside Run
    lifecycle. One column when narrow.
  - **Reads only while shown.** `App` widens the monitoring visibility gate (BE-5) to the
    workspace's Live (`workspaceLiveVisible`), with the 500 ms snapshot poll while shown (200 rows;
    the sorter trigger's status only for the classic tab). The switch is the instrument's, shared by
    every client: `monitoring_set_active(true)` while Live or the classic Monitoring tab is shown;
    `false` when neither is, but only once the run status has been read and shows no run in progress
    (`runInProgress`: Starting, Active, Stopping or flushing, not terminal). During a run nothing is
    sent on leaving, so the controller moving to Plan or Prepare never cuts the feed viewer clients
    watch; when the run ends off Live, `false` follows. Only the controlling client sends it (a
    viewer only reads). The command only sets `ProcessingService`'s monitoring flag, which gates the
    monitoring rings' appends (and so the cell strip fed from them); it does not touch acquisition,
    recording, persistence or the run, and turning it off clears nothing. Live reads `fetch_monitoring_cells`
    once a second (the backend refreshes its strip on that read) and pulls each shown cell's crop
    once (`fetch_monitoring_cell_packet`, kind 5; the mask, kind 6, only for the Mask overlay), at
    most two at a time, for the 12 newest strip cells and the pins; a cell that leaves the listing is
    forgotten. The live frame is the one the classic preview already pulls (`subscribeFrames`, the
    same single sink Align uses), painted once per animation frame at most.
  - **The strip** is the kit's `CellStrip` / `CellImage`, newest first. A live cell has no record id
    yet, so it is named by frame and object ("F41118·O1"). A crop still loading is an empty box (the
    kit's missing state says "Image not recorded", which would be untrue; a loading state is asked on
    #715); a cell without an image (the PZ7035's PL reports results only) or a crop refused as another
    capture's (`CAPTURE_MISMATCH`, never retried, the listing read again) shows the kit's missing
    state with the reason as its tooltip.
  - **Click-to-pin** from the strip or the scatter, only by the controlling client: one
    `monitoring_cell_pin` with the cell's or row's identity; the reply's outcome is said in plain
    words with the backend's message (`pinNotice`: Pinned, PinnedWithoutImage, AlreadyPinned,
    RunMismatch, NotFound, LimitReached with "ended pins count toward the limit too",
    CaptureMismatch "pick a cell of the current capture"). The scatter is the kit's `AreaDeformChart`
    with the classic chart's rows (valid, area in µm² with each row's own analysis-time px→µm; the
    uncalibrated ones are counted), axes widened in whole steps when a point lies beyond 300 µm² /
    0.20. A click maps to the nearest point within 8 px through the kit's plot margins
    (`CHART_MARGIN`; `liveModel.test.ts` renders the kit chart and checks every point lands where the
    mapping says) until the kit offers point selection (#715). A row without an identity (an older
    backend) is not pinnable: nothing is sent and the view says so. Nor is an identity of another
    capture or run than the listing's (`pinIdentityRefusal`, the backend's own CaptureMismatch /
    RunMismatch rule): refused before anything is sent, with the reason.
  - **The pinned cell**: the selected pin (the newest, or the last one pinned) with its crop, facts
    (area µm², deformability, E, the image's state, run and capture), ◀ / ▶ through the pins, **Open
    in window** (a modal dialog over the workspace, with Raw / Mask, ◀ Previous / Next ▶, the arrow
    keys and Escape; not a second window, which would need a new Tauri window and capability in the
    shared `src-tauri`), **Unpin**, **Clear ended pins (N)** (one `monitoring_cell_unpin` per pin of
    an ended capture; there is no backend command for "ended only"; control is rechecked before each,
    and a control loss cancels the rest for good, even if control comes back before the next one:
    the operator presses again) and **Clear all pins**. The count
    chip says "N of 16 pins" and "N ended". A pin without an image (an ended capture's, or a row that
    had left the buffer) shows an app-local box with the reason, never a picture; its measurements
    stay. Pins are one shared, bounded set per instrument: without control the strip's crops are not
    buttons, the scatter takes no click, the pin buttons are disabled with the reason, and the hook
    sends nothing even when asked.
  - **Freeze display** holds the live frame, the strip and the scatter as they were (no crop is
    pulled for the held strip); pins and the run status keep updating; nothing is sent. A listing
    of another capture or latest run drops the held strip and its crops even while frozen, and the
    frozen notice says the capture changed; the scatter stays held, and its earlier capture's points
    are refused as above.
  - **The stops are separate.** "Stop & finalize run" (`stopTarget`): in a queue that has begun,
    only the queue's own row, through `queue.stopCurrent` (as Next sample: `experiment_stop` with the
    row's `start_generation`), never while the queue is paused, and never another run (that one is
    stopped in the classic view); otherwise the run the status shows Active, by its
    `start_generation` (App's `stopForLive`, ABI 40), a RunMismatch reply said and nothing more
    sent. "Stop acquisition" is the classic Stop Camera (`onStopCamera`, a recording clip first),
    disabled while a run is active ("the camera never stops under a run") or the camera is not
    running. Both need control.
  - **Notices follow the state (#763 F13, F15).** "Stop requested · run generation N" goes once the
    status shows that run terminal or another run (`stopRequestStale`); a refused stop stays until
    the next stop. A notice that describes a pin ("Pinned · F…") goes once that pin's capture ended
    or a listing at least as new as the pin's reply no longer holds it (`pinNoticeStale`). A run
    start drops every pin (pins belong to the latest run); while the capture that run began is
    current, Live says how many were cleared ("1 pin cleared · run generation 4 started",
    `droppedPinsNotice` from `last_capture_change.pins_dropped`), until dismissed.
  - **Run status and lifecycle.** The run's file, a state chip and the backend's counts (valid and
    invalid saved, pending, dropped, failed to save). The queue's tiles show each row's chip (a
    single sample's follows its run by the experiment operation its start reply named,
    `StartResult.operation_id`, `followSingleRun` / `singleRunChip`: Started, then Finalized · file
    closed, Finalized · cancelled or Failed from that operation's terminal OperationStatus event,
    kept after another run starts; #763 F14). The backend finishes that operation when the run's
    status turns terminal, so its end is this run's and no other's. The status is not used for the
    sample: the start reply returns no start generation, and a status on the same file or with a
    newer generation can be another client's run (Codex #766 review). A reply without an operation,
    or events possibly lost since the Start (`OperationTrack.loss`: a notification overflow, a failed
    poll, a dropped server connection) before the end was seen, reads "Outcome unknown · check the
    file"; an end that still arrives is taken. Running and draining are shown by the run card, not
    attributed to the sample. Binding the sample to the status as well would need the start
    generation in the start reply (a contract field, not added); the
    lifecycle is the queue's own (`lifecycle`, with Confirm closed for a finalized row and "Go to
    Plan" for the next Start, which stays in the Plan with its file dialog and readiness) or, for
    any other run, `statusLifecycle`: Starting → Running → Stopping · drain → Finalized, a step done
    only once the status reports the next one. A control loss pauses the queue (the paused notice
    and Resume are in Live too); nothing resumes on its own and resuming sends nothing.
  - Not in this step: the population selection card (live populations are P5 / #523), Mark chapter
    (chapters are deferred, ADR 0015), Inspect (Review), the stop-condition progress (per-run
    auto-stop is not built) and the footer's camera / display / transport / writer line.
- **Gate (P1, extended for P3).** `workspace/navigation.test.tsx` runs the whole App over a
  mocked transport, desktop and PZ7035: opening the workspace, visiting every stage, leaving,
  coming back and Camera setup add no command to the idle app's steady state except two reads,
  `fetch_experiment_readiness` with `outputPath: ""` and Align's `fetch_camera_geometry`, and
  nothing that acts or writes (ROI, config, script, camera selection writes are on the list);
  visiting Align sends no mode switch, pressing its button sends exactly the classic command,
  typing in a quick control writes nothing and Write sends `save_camera_roi` (desktop) or
  nothing (the PZ7035 Run window); from the classic Experiment tab, Align's switch then
  "Classic view" sends nothing that acts (desktop and PZ7035, the PZ7035 with a Run window
  placed in Align), and only the classic notice's button switches back; a classic tab change whose
  geometry read is held until after "Workspace preview" sends nothing that acts once the read
  answers, and the classic view then shows the notice (this case failed before the cancellation).
  A control shows the classic Camera & Alignment tab does send
  `set_camera_overview` / `set_instrument_mode`. For the Plan (A5), visiting it, typing in the
  sheet and switching its sub-views add only the path-less readiness read and nothing that acts
  (desktop and PZ7035, where the mock's checklist blocks and Start run says so); on a desktop host
  that can start, "Start run" sends exactly one `experiment_start` with the plan's
  `startRequestJson`, the classic Start Experiment still sends none, and an `InvalidRequest`
  reply is shown next to the sheet with the plan kept. After review (#736): a successful start
  followed by failing status reads keeps the plan locked (no second `experiment_start` after the
  reads recover); with the core status read held, several periods send one read, not a pile, the
  read after the stale bound is the only extra one, and the stalled older reply arriving last
  does not overwrite the newer state; Live and Summary send neither the gates nor the checklist
  reads, and Plan resumes them. Since A6b, showing Live adds only its reads and the monitoring
  switch (`LIVE_COMMANDS`: `monitoring_set_active`, `fetch_monitoring_snapshot`,
  `fetch_monitoring_cells`, `fetch_monitoring_cell_packet`), and monitoring is off again once Live
  is left with no run in progress. With S1 running, leaving Live for Plan, Prepare or the classic
  view sends no `monitoring_set_active(false)` (nor does leaving the classic Monitoring tab); S1's
  end off Live sends it once; a viewer sends none at all, and on taking control it follows the view.
  With S1 running, Live's control loss pauses the queue and sends nothing (Stop & finalize
  and Stop acquisition disabled), control back keeps it paused, Resume sends nothing, and Stop &
  finalize then sends exactly `experiment_stop` with S1's generation; Confirm closed sends nothing. For the sample queue (A5, desktop and PZ7035): choosing the queue,
  adding, typing, moving and removing samples and switching sub-views send nothing that acts (only
  the path-less readiness read), and the queue survives leaving the workspace; on a desktop host
  whose mock runs experiments, two samples send exactly `experiment_start` (S1, run 1),
  `experiment_stop`, `experiment_start` (S2, run 2), `experiment_stop`, each on its button and
  nothing in between (not on the finalize, the close confirmation or the swap prompt); on the
  PZ7035, visiting the Plan sends no switch, "Switch to Run" is disabled until the Run window is
  placed in Align, then sends exactly `set_instrument_mode` run at the placed window. After review
  (#739, `useHoldsControl` mocked as a store the test flips): losing and regaining control while
  the Start's readiness read is held sends no `experiment_start`, and Start after Resume sends one;
  when S1's post-start refresh fails and another client's run (its own file) is reported first, S1
  is interrupted, never adopted, and nothing is stopped or confirmed; after Align's "Show the full
  sensor", Start (queue and single) is disabled with the reason, navigating sends nothing, and "Show
  the experiment window" sends exactly `set_camera_overview` false. With the mock answering as an
  ABI 40 backend (#741: start and stop outcome codes, `expectedStartGeneration` compared with its
  run): the two samples' stops carry `{expectedStartGeneration: "1"}` and `"2"`; another run
  started between the last read and Next sample gets exactly one stop naming run 1, is not
  stopped, S1 is "Interrupted · not stopped" with the backend's reason and nothing more is sent;
  S1's run that ended by itself is refused ("no run is active") and still followed to Finalized;
  the classic Stop Experiment sends no generation; a reply without codes is handled as before; the
  InvalidRequest is shown next to the sheet by its code and, without one, by its prefix; a
  StorageFailed reply locks the single plan. `experimentStart.test.ts` covers the withdrawal at
  each step, and every start code of the contract (written out, so a change of the mapping or a
  new code fails), a code against `ok`, an unknown code and the reply without one.
  `queueModel.test.ts` covers every transition, a failure at each step, retry / skip / end, the
  generation rules, the pause, editing locks, the stepper and a 50-trial sequence check that no run
  index is sent twice, and (ABI 40) RunMismatch, Busy, NotActive and no-code stop replies, a
  locking start code and a refusal code with a misleading message; `QueuePlan.test.tsx`
  (StrictMode, the real `usePlan`) the two-sample flow, the exact text of each Start, each failure's
  choice, the lost-client pause and resume, the Run switch, and (ABI 40, through the real
  `startExperiment`) every start code's row state, the exact generation of each stop and the
  RunMismatch row; `PlanView.test.tsx` also locks the single plan on each locking code. `serialReads.test.ts` covers the skip, stale and ordering rules. `PlanView.test.tsx` (StrictMode, the real
  `usePlan`) covers the exact text sent, client refusals, outcomes, Prepare's and the classic
  locks, and the started lock; `planModel.test.ts` the exact JSON of full, minimal and empty
  plans, every bound at its limit and one past it, empty → absent, the UUID and the refusal
  parser; `experimentStart.test.ts` the pass-through. `Workspace.test.tsx` proves Prepare polling
  never passes a path and Align opens without a command; `AlignView.test.tsx` (StrictMode)
  covers the image tools on delivered frames and the write states; `prepareModel.test.ts`,
  `alignImageModel.test.ts`, `quickControlModel.test.ts` and `alignControls.test.ts` cover the
  derivations.
- **Not yet (later A-steps):** the other Prepare sub-views, storage budget and run plan,
  fluidics estimates, the non-default-settings banner, the calibration card; in Align the focus
  and illumination card, the quality signals, All Features / GenApi XML / scripts, selector
  enumeration and restoration, and overrides recorded by S2 (#712); in Experiment per-run
  auto-stop (a backend stop condition), "base on previous experiment", the queue preview, Live
  (A6), Summary and Quick look (A7).

**Quality gates (UX-4 slice, issue #308):** `desktop/src/quality.ts` —
`deriveQualityGates(input)` turns the Camera & Alignment stage's
"is the image good?" into concrete gates (focus / background / ROI /
calibration), each `pass`/`warn`/`fail`/`unknown` from bridged signals
(autofocus ring-ratio + freshness, config background/ROI, frame size, px→µm).
Rendered as a strip under the live image in the Overview tab. Illumination,
channel-wall ROI insets (#295), Auto-Focus, and save-to-profile (UX-2 #306)
are follow-ups — gates only report what the backend exposes. Details:
`knowledge_map/task/2026-07-21-ux4-quality-gates.md`.

**Active-context bar (UX-8, issue #312):** `desktop/src/contextBar.ts` —
`deriveContextBar(facts)` builds the persistent bottom bar shown on every stage:
Profile / Camera / Calibration / Status / Operator / Storage / Warnings, each a
value + `ok`/`warn`/`blocked`/`pending`/`neutral` status + optional navigate
target. Status mirrors the guided-workflow readiness; Warnings counts preflight
+ quality attention items; Camera/Calibration come from the bridged selection +
px→µm. Profile (UX-2 #306), operator identity, and storage free-space are
`pending` until bridged — shown explicitly, never faked. Details:
`knowledge_map/task/2026-07-21-ux8-context-bar.md`.

**Operator / Service-Commissioning mode (UX-9, issue #313):**
`desktop/src/commissioning.ts` — `canActuate/canStartPeriodic/canStopPeriodic`
gate hardware-actuating trigger tests. Every session starts in `operator` mode
(`DEFAULT_MODE`); a menubar toggle enters `service` mode behind a confirm, with
a persistent banner. The Monitoring trigger controls (Sort Trigger, Set Pulse,
Periodic Test) render only in service mode behind an **Arm** checkbox, gated by
those checks; arming is one-shot and clears on mode exit or an active
experiment. A running periodic test stays stoppable in operator mode as a safety
fallback. Details:
`knowledge_map/task/2026-07-21-ux9-commissioning-mode.md`.

Native **file pickers** use `tauri-plugin-dialog` (registered in `run()`,
granted via `dialog:default` in `capabilities/default.json`, called from the
frontend through `@tauri-apps/plugin-dialog`'s `open`/`save`): a folder picker
for the mock frame dir, a save dialog for the recording path, and an open dialog
(HDF5 filter) for the review file — so paths are never hand-typed.

**Platform services (BE-9, #279):** `src-tauri/src/platform.rs` provides
stable app paths (`app_paths`), persisted shell preferences
(`get/set_preferences` — one JSON document in the app-config dir, atomic
writes), and a webview log sink (`shell_log` →
`<app_log>/desktop-shell.log`). `src-tauri/src/updater.rs` verifies update
manifests fail-closed (SHA-256 pinning, unit tested). Native open-URL /
reveal-in-dir actions go through `tauri-plugin-opener`, capability-scoped to
`https://**` and directory reveals only.

## Remote server (YOFO Studio)

`crates/mib-bridge-server` (`yofo-studio-server`) runs the same command layer
in a headless process for a browser UI, e.g. on the PZ7035 PS (ADR 0008, impl
spec S5). Protocol on `/ws`, token on the upgrade (`?token=` or
`Authorization: Bearer`, from `/etc/yofo-studio/token`; `--no-token` only on
loopback):

**Who controls the instrument (#501).** In a browser one client at a time controls the instrument; the
others only watch and their commands fail with `VIEWER_ONLY`. The server sends each client
`{"session": {"client_id", "controller_id"}}` on connect and `{"session": {"controller_id"}}` when control
changes; `wsTransport` keeps it (`transport.session`, reset on a lost link) and `useControlRole` exposes it
(`useHoldsControl` too: this client controls the instrument; the workspace's sample queue pauses without it).
`ControlBanner` (in `AuthGate`) tells a viewer "Another client controls this instrument: you can watch,
not operate." with a **Take control** button (`take_control`), which disappears for the new controller and
appears for the old one. Without it a second browser, or an older tab that reconnected first after a
restart, showed buttons that failed with only a log line (seen on the board on 2026-10-08).
The classic view also disables those buttons for a viewer (#763 F19): `components/ViewerGate.tsx`
wraps each cluster of controls that sends a control command in a `fieldset.viewer-gate`
(`display: contents`, so toolbars keep their layout) that is `disabled` for a viewer, with the
reason as its title; single buttons inside a `<p>` (Resume Run, the Run Stop, the config Apply
buttons) take `viewerOnly` in their own `disabled`, and File ▸ Open Recording… is `pending`.
Navigation, view toggles, reads and the ring playback stepping stay open to a viewer. The desktop
shell has no session, so nothing changes there. `ViewerGate.test.tsx` also checks App.tsx's
control-command controls against the gate.

**Reconnecting (#501).** `AuthGate` keeps probing `/auth` every 2 s after the app mounted. The
server reports a `boot_id` (one value per server process; `mib-bridge-server` `Server::new`) on every
`/auth` answer. Two missed probes show "Connection to the instrument lost. Reconnecting…"; when the
server answers again with the same boot id the page just carries on (the WebSocket reopens by
itself on the next call); with a different boot id (backend restarted, board rebooted) the app is
re-mounted, so it loads the new backend's state, and a dismissible notice says so, all without a
page reload. A 401 (a new token after a restart) puts the token prompt back over the running app,
using the stored token first. An older server without a boot id never triggers a re-mount. Tests:
`components/AuthGate.test.tsx`, `mib-bridge-server` `auth_probe_reports_the_token_without_a_socket`,
and a Chromium run that stops and restarts a real server under an open page.

- request `{"request_id", "cmd", "args"}` with the `invoke` name and camelCase
  arguments; reply `{"request_id", "ok"}` / `{"request_id", "error"}`;
- binary replies: 8-byte little-endian request id, then the unchanged bytes
  (MIBF frame packets); frames stay client-pulled;
- events: the server alone drains the backend queue every 20 ms and pushes
  `{"event": EventEnvelope}` to every client; `poll_events_exact` from a client
  fails with `SERVER_OWNS_EVENTS` (two pollers would steal each other's
  events). Live frames emit no FrameReady (they are pulled);
- `init` is idempotent; commands of one connection run in order, each on a
  blocking thread;
- client loss: pings every 2 s, a connection silent for 5 s is dropped; when
  the last client has been gone for 5 s the server stops and saves (active
  experiment -> `experiment_stop`, raw recording -> `stop_recording`, busy Z
  stage -> `stage_stop`, PZ7035 in Align/Run -> idle, and last every
  connected pump -> `pump_stop` + `pump_stop_purge` (#711); capture keeps
  running). The desktop close guard refuses to close instead; a remote
  operator who lost the link cannot see the run. SIGTERM does the same, then
  shuts the backend down. `/healthz` reports clients and passes.

The desktop-only platform commands (`app_paths`, `get_preferences`,
`set_preferences`, `shell_log`) are answered from the server's data
directory (`config/preferences.json`, `logs/remote-shell.log`), shared by all
clients.

**Frontend transport.** Every call site imports `invoke` from
`desktop/src/transport` instead of `@tauri-apps/api/core`: inside Tauri it is
Tauri IPC, in a browser `wsTransport` (socket at `/ws` of the page origin, or
`?server=`; token from `?token=`; `VITE_MIB_TRANSPORT=tauri|ws` forces one;
unit tests use the mocked Tauri API). `wsTransport` returns binary replies as
`ArrayBuffer` like Tauri, answers `poll_events_exact` from the pushed
envelopes (so the event loop is unchanged), reconnects on the next call and
rejects calls in flight with `TRANSPORT_LOST`. `upload(cmd, bytes, headers)`
sends a raw request body (the review module's chart snapshots, #512 S9): in
Tauri `invoke(cmd, bytes, {headers})`, the raw IPC body; over the WebSocket it
rejects with `TransportUnsupportedError` (`code` `TRANSPORT_UNSUPPORTED`,
`isTransportUnsupported`) without sending anything, because the server reads
text requests only. Callers show their own words for it. `transport/dialogs` replaces the
dialog/opener plugins: native in Tauri, prompts for instrument paths and
`window.open` in a browser (`ask`, the Yes / No question, is
`window.confirm` there). A browser `save` prompt (#762) fetches
`fetch_instrument_status` as it opens (every server answers it, PZ7035 or
not; the last known directory is the fallback), defaults to
`<storage.path>/<suggested name>` and says where the file goes ("The file is
saved on the instrument, in <dir>. A relative path is saved there too."); the
backend resolves a relative output path under the same storage root
([[ExperimentCoordinator]], "Output path"). `open` prompts keep the
suggested default under the last known directory. The review module (`desktop/src/review/`) uses
the transport too ([[../frontend/YofoReview]], "Transport"). In a browser the close guard only warns on
`beforeunload` (the server owns stop-and-save) and the installer updater is
hidden. Verified in headless Chromium: the full UI, live mock frames at 30 fps
over the socket.

**One controller.** The first client controls the instrument; others are
viewers whose instrument-changing commands (`CONTROL_COMMANDS` in the server)
fail with `VIEWER_ONLY`. `take_control` claims control; it passes to the
oldest remaining client when the controller disconnects. Every client gets
`{"session": {"client_id", "controller_id"}}` on connect and on each change.
Test: `one_client_controls_the_instrument`.

**Packaging.** `scripts/yofo/stage_image.sh` builds the ARMv7 backend, the
server and the UI and stages them for pz7035-imx426's `yofo-studio` recipe
(meta-yofo), which installs `/usr/bin/yofo-studio-server`,
`/usr/share/yofo-studio/dist` and `yofo-studio.service` (port 8427, token
generated on first boot in `/etc/yofo-studio/token`, data in
`/var/lib/yofo-studio`). The backend is not built by BitBake because it needs
the SDK of the same image.

**Science on the PL.** After `init` the UI reads `fetch_platform_info`; with
`host_processing` false the realtime switch, backgrounds, calibration and the
processed preview are hidden and the sidebar says processing runs on the PL
(ABI 21, [[Rust-Bridge]]).

On the PZ7035 PS (2026-10-01): the ARMv7 server with `MIB_CAMERA_MODE=aravis`,
`MIB_ARAVIS_FPS=1000`, `MIB_ARAVIS_EXPOSURE_US=900` served the UI to a browser
on the bench PC, which showed live lit 512x96 IMX426 previews at the UI's
30 fps pull rate. Server footprint: ~2 % CPU and 12-15 MiB RSS idle; with
capture running ~90 % CPU (the backend takes every preview the producer
delivers, ~400/s, while the UI shows 30/s) and 266 MiB RSS after a UI session
(desktop-sized buffers; a target profile is open in impl spec S6).

`desktop/dist` is served at `/` with `--dist`. `/favicon.ico` is the UI's own
`dist/favicon.ico` when it has one, else 204 No Content (the UI ships none, so every
page load logged a 404; #763 F20; test `favicon_is_the_ui_icon_or_no_content` in
`tests/files.rs`). Tests: `tests/ws.rs` (mock
capture over the socket, wrong token refused, client loss and quick reconnect,
client loss with a pump on a stalled serial adapter, #720).

## Camera & Alignment (Qt Overview parity)

With a camera that has an overview (MindVision, Aravis/PZ7035), entering the
Camera & Alignment tab calls `set_camera_overview(true)` and entering
Experiment `set_camera_overview(false)`, as Qt's tab change does; other tabs
leave the camera alone and nothing changes during a run. The tab then shows
the whole sensor with the experiment window as a yellow box (drag to move;
release saves, as Qt saves on move) and X/Y/W/H fields with "Save camera ROI";
`cameraAlignment.ts` snaps to the camera's steps and states "Sensor N Hz (max,
limit) -> >= M images/s here (limit, bands)". For other cameras the fields keep
setting the processing ROI. Verified on the PZ7035 through the browser:
816x624 lit overview, window dragged to (232, 356) and saved, Experiment
showed that 512x96 window. The browser shows the full field at ~26 fps (all
the producer delivers at the 830 Hz preset) and the preview at the 30 fps
display rate. Before `bytes_to_vec` (see [[Rust-Bridge]]) the full field was
held at ~10 fps by per-byte frame copies in the bridge.
`vitest` discovery is limited to `src/` (`vite.config.ts`): crawling
`src-tauri/target`'s cxx symlink loop hung `vitest run`.

## Command layer

Thin wrappers over the bridge (all take the managed `AppState`; bodies in
`crates/mib-app-commands`, Tauri shims in `desktop/src-tauri/src/lib.rs`):

- **Live capture:** existing lifecycle commands remain serialized through
  `AppState.bridge`. `fetch_frame_packet` returns one owned binary response.
- **Recording/review:** `fetch_indexed_frame_packet(frame_index)` and
  `fetch_review_frame_packet(dataset,index)` accept canonical decimal-string
  indices. `fetch_background_packet` uses the same codec.
- **Review (ADR 0014):** `src-tauri/src/review.rs` — `review_open/close`,
  `fetch_review_info/rows/frame/series_*/thumbnails_packet/scatter`,
  `review_save_core_record`, `poll_review_events`, `cancel_review_operation`
  over the review bridge ([[Rust-Bridge]]); the only commands the
  `review-only` build registers besides `init`/`is_initialized`/
  `abi_version` (review-bridge versions) and `platform::*`. Backend-bridge
  commands are `#[cfg(not(feature = "review-only"))]`.
- **Compatibility:** old split-cache commands return
  `FRAME_PROTOCOL_UPGRADE_REQUIRED`; they cannot return a substitute image.
  C++ ABI 11 is unchanged; desktop frame wire protocol v1 is independently
  versioned in `bridge-contract.json`.

`framePacket.ts` validates the complete packet before canvas allocation. Frame
indices/timestamps are decimal strings. Missing source/session/config identity
and timestamp clock validity are explicitly unavailable, so identity-matched
scientific overlays remain blocked pending the accepted backend contract.

`FramePullScheduler` owns at most four views, one aggregate in-flight pull,
and one replaceable pending request per view (no pending pixel buffers).
Review intents supersede older replies; live sampling does not starve a slow
in-flight frame. Unmount/navigation/source changes retire replies without
stopping a run or claiming native cancellation. A reply that arrives after the source
changed rejects with `FRAME_REPLY_STALE`, and a pull during a source switch (`bridge.ts`
`sourceMutation`, e.g. Review opening a file) with `FRAME_SOURCE_CHANGING`
without reaching the backend. Both are the client's own discards, so the
classic view's scheduler mounts skip them (`isFrameReplyRace`) and log only
backend refusals (#763 F20). No pixels enter React state.
Live sampling targets 30 Hz; routine polling and metadata state updates are
capped at 5 Hz. These are scheduling policies, not measured camera throughput.
State polling no longer awaits the image response, but native lock contention
and webview stalls still need native measurement.

See `docs/architecture/frame-packet-v1.md` and
`docs/exec-plans/active/2026-09-07-agent-b-react-tauri.md` for byte budgets,
explicit backend prerequisites and executed versus pending evidence.

## Build & run

- Frontend: `npm install && npm run build` in `desktop/` → `desktop/dist`
  (Tauri's `frontendDist`): two pages, `index.html` (MIB Studio) and
  `review.html` (YOFO Review). `tsc` typechecks under strict mode.
- UI language kit: `desktop/src` imports `@mib/ui` (and `@mib/ui/style.css`,
  `@mib/ui/fonts.css`) from source through `resolve.alias` in `vite.config.ts`
  and `paths` in `tsconfig.json`; no kit build step. See
  [[../frontend/UI-Language-Kit]].
- Version: `tauri.conf.json` and `package.json` carry the repository
  version, stamped by `scripts/release/stamp-tauri-version.py` from
  `cmake/MIBVersion.cmake` (`--check` runs in `review-ci.yml`).
- App: `cargo build` in `desktop/src-tauri` (needs `dist/` to exist — Tauri
  validates `frontendDist` at compile time). Links the bridge via
  `MIB_BRIDGE_NO_CMAKE=1` when the archives are prebuilt.
- Crate-type is **`rlib`** only (binary + rlib, an executable). The mobile
  `cdylib`/`staticlib` types are omitted because the non-PIC C++ archives can't
  link into a shared object — see [[Rust-Bridge]].

## Headless verification

Two gates, both without a real display:

1. `cargo test` — `mock_camera_slice_round_trip` drives init → configure → start
   → pull-frame (asserts 512×96) → stop → shutdown directly on the bridge from
   the desktop crate; `record_and_review_round_trip` drives record → load → seek
   by index → pull-by-index; `processing_settings_round_trip` drives
   apply-processing → pull-stats (asserts the px→µm scale round-trips); plus
   `event_kind_names_are_stable`.
2. `xvfb-smoke.sh` — launches the real binary under Xvfb with the container
   WebKitGTK workarounds (`WEBKIT_DISABLE_DMABUF_RENDERER=1`,
   `WEBKIT_DISABLE_COMPOSITING_MODE=1`, `LIBGL_ALWAYS_SOFTWARE=1`) and asserts
   the window + webview come up and stay alive. This is what makes the GUI
   verifiable in headless CI (`desktop-ci.yml`).

## Gotchas

- WebKitGTK needs the dmabuf/compositing env workarounds to initialize in a
  container; without them GTK init fails headless. `xvfb-smoke.sh` sets them.
- `dist/` must exist before `cargo build` — build the frontend first (CI does).
- Keep frame pixels in the atomic versioned binary packet; do not JSON/base64
  them through `poll_events` or a command return (ADR 0003 hot-path rule).
- **Debug binaries load `devUrl`, not `dist/`** — Tauri embeds
  `build.devUrl` in dev profiles, so running `target/debug/mib-studio-desktop`
  without `npm run dev` on :1420 shows "Could not connect to localhost". The
  Xvfb smoke therefore only proves the shell boots; driving the real UI
  headless needs Vite running (or a release build, which embeds `dist/`).
- **Vite must not watch `src-tauri/`** — cxx-build creates a `crate` symlink
  loop under `src-tauri/target/**/cxxbridge` that crashes Vite's watcher with
  `ELOOP` seconds after startup. `vite.config.ts` sets
  `server.watch.ignored: ["**/src-tauri/**"]`.
- An **empty `data_dir`** passed to the `init` command resolves to Tauri's
  `app_data_dir` — `AppBackend::initialize("")` rejects an empty path (found
  by driving the UI under Xvfb: init always failed before this).

## Exact event contract continuation (ABI 12)

The desktop now uses versioned `poll_events_exact` JSON and one named event
adapter. Commands/experiment snapshots retain u64 identities as decimal strings;
legacy cxx slots are preserved with exact companion fields where floats previously
lost precision. Processing metrics preserve unavailable/non-finite values instead
of zero; unknown clock domains cannot produce elapsed-time claims. See
`docs/architecture/event-json-v1.md` and
[[../task/2026-09-07-agent-b-event-contracts]] for executed evidence and gaps.

## Experiment status and readiness (ABI 13, issue #372)

`fetch_experiment_status` returns the shared coordinator's full status
(`start/readiness/capture_generation`, `persistence_*`, `terminal`,
`finalization_ok`, `completion` as a `run_completion_states` value,
`completion_reason`, `fault_code/message`) and the new
`fetch_experiment_readiness(outputPath)` the gate list
(`readiness_gate_statuses`; Fail/Unavailable block) with the generation a
Start must present. The event JSON gains the typed companions listed in
`docs/architecture/event-json-v1.md`; `experiment_completion` is a decimal
string like every enum slot. `eventAdapter.ts` decodes both
(`decodeExperimentStatus`, `decodeExperimentReadiness`) and refuses unknown
completion / gate-status values; `bridge.ts` exposes
`fetchExperimentReadiness`. Guards: `eventAdapter.test.ts` (golden decode
with typed fields, readiness gates, unknown enum refusal),
`event_transport::tests::cpp_rust_json_matches_shared_golden`.

**Start request (ABI 39, #712).** `experiment_start` takes an optional
string `startRequestJson` (Tauri command arg `start_request_json`, `dispatch.rs`
`startRequestJson`, `bridge.experimentStart(path, ack, startRequestJson?)`; the
document's shape is `ExperimentStartRequestDoc`).
`mib-app-commands` passes the text to the bridge byte for byte, `""` when absent
or null; it is never decoded in Rust or TypeScript, so duplicate keys and the
byte bound reach the backend parser unchanged. Nothing is checked in Rust or TypeScript:
the backend parses it strictly and refuses with `invalid start request: <key>`
([[Rust-Bridge]] ABI 39). The workspace's Experiment · Plan sends it (#716 A5, "Opt-in
workspace" above): `planModel.buildStartRequest` writes the text with one ordered serializer and
checks it first against `bridgeContract.ts` `EXPERIMENT_START_REQUEST`, so an over-bound plan is
refused before it is sent; the backend still checks it and stays the authority. The classic Start
sends none (the context bar's `operatorName: ""` stays).

**Typed outcome and the run a stop names (ABI 40, #741).** `CmdResult`
(`eventAdapter.ts`) gains `start_outcome?` (an `EXPERIMENT_START_OUTCOMES`
value) on an `experiment_start` reply and `stop_outcome?` (an
`EXPERIMENT_STOP_OUTCOMES` value) on an `experiment_stop` / `experiment_cancel`
reply. `decodeCommandResult` leaves them out when the reply has none (an older
backend, any other command) or a code this UI does not know (fall back to
`ok` / `message`) and refuses a non-integer (`TRANSPORT_INVALID_OUTCOME`).
Callers switch on `start_outcome === EXPERIMENT_START_OUTCOMES.InvalidRequest`
instead of the `invalid start request: ` prefix, keeping the prefix only as the
fallback for an older backend. `bridge.experimentStop(expectedStartGeneration?)`
sends `{expectedStartGeneration}` as a canonical decimal string (`decimalU64`;
the `fetchExperimentStatus().start_generation` of the client's run) and no
argument without one; another run, or none, comes back `ok: false`,
`stop_outcome: RunMismatch` (3) and nothing is stopped ([[Rust-Bridge]] ABI 40).
The Tauri command takes `expected_start_generation: Option<String>`. The
develop shell (`App.tsx`, `experimentStart.ts`) neither parsed the prefix nor
passes a generation, so it was unchanged by the contract step; the workspace's
queue and Plan switched on `feat/egrabber-workspace` (#741 part 2, "Opt-in
workspace" above: `startReplyState` maps each start code to started / refused /
unknown, and only the queue's Next sample / Stop last sample pass the row's
generation; the classic Stop Experiment sends none). Guards:
`eventAdapter.test.ts`, `eventTransport.test.ts`, and for the workspace
`experimentStart.test.ts`, `queueModel.test.ts`, `QueuePlan.test.tsx`,
`PlanView.test.tsx`, `navigation.test.tsx`.

**The applied method's calibration declarations (ABI 43, #776).**
`bridge.fetchExperimentReadiness()` now returns `method_calibration`
(`MethodCalibration` in `calibrationRecords.ts`, decoded strictly by
`decodeMethodCalibration` from `eventAdapter.decodeExperimentReadiness`):
`declared`, `purpose` (`measurement` | `calibration`), `calibration_policy`
(`none` | `warn` | `required`), `calibration_max_age_hours`,
`calibration_reference` (`nominal_diameter_um`, `diameter_tolerance_um`,
`nominal_e_kpa | null`, `max_cv_percent`; non-null exactly for a calibration
method) and `error`. An unknown purpose or policy, a missing member, a number
outside the method document's bounds, or a reference that does not match the
purpose is refused (`CALIBRATION_REPLY_INVALID`).
A reply without the member (a backend before ABI 43) decodes as `null`: unknown,
never assumed to be a measurement method. The workspace Prepare Calibration card
reads it ("Opt-in workspace" above); no classic UI does. Guards:
`calibrationRecords.test.ts`, `eventAdapter.test.ts`.

**Calibration records (ABI 42, #716 A4a).** `calibrationRecords.ts` types and
checks the replies of [[../services/CalibrationRecords]]:
`bridge.calibrationRecordsList()` (`calibration_records_list`: the newest version
of every record, newest first, a plain list, with the applied and in-effect ids,
the current factor and identity), `bridge.calibrationRecordGet(recordId)` (one
record and its versions) and `bridge.calibrationApply(recordId,
expectedCurrentFactor)` (the compare-and-set; the reply has `previous_factor` /
`new_factor` and an `outcome` from `CALIBRATION_APPLY_OUTCOMES`). An unknown
status or outcome is refused (`CALIBRATION_REPLY_INVALID`); a malformed id or
factor throws before anything is sent (`INVALID_RECORD_ID`,
`INVALID_EXPECTED_FACTOR`). Apply is a control command on the remote server
(`VIEWER_ONLY` for a viewer); list and get are reads. The startRequest's
`calibration_record_id` is now a check against the applied record in effect.
No develop UI uses them; the workspace Prepare card (A4b) will. Guard:
`calibrationRecords.test.ts`.

**Live cell images (ABI 41, #716 A6a).** `monitoringCells.ts` types and checks
the backend's live cell store ([[../services/MonitoringCellStore]]):
`bridge.fetchMonitoringCells()` (`fetch_monitoring_cells` → the strip of recent
cells and the pins, `MONITORING_CELLS_INVALID` on a reply that breaks the
contract or its caps), `bridge.monitoringCellPin(key)` (args
`{expectedStartGeneration, captureEpoch, frameIndex, objectId}`, a pin reply with
`outcome` from `MONITORING_CELL_PIN_OUTCOMES`, `CaptureMismatch` (6) when the key names
another capture than the current one; an unknown outcome is refused),
`bridge.monitoringCellUnpin(key)` and `bridge.monitoringCellClearPins()` (a
`CmdResult`), and `bridge.fetchMonitoringCellImage(key, "image" | "mask")`
(`fetch_monitoring_cell_packet`, a frame packet of pull kind 5 / 6 decoded by
`decodeFramePacket`; an empty packet when the layer is not held; a rejection
`CAPTURE_MISMATCH: <reason>` for an identity of another capture, read with
`monitoringCellCaptureMismatch(error)`: the cell is gone, refresh the listing).
The listing carries `last_capture_change` (why the strip was cleared and the
pins of the capture that ended kept their measurements only, with
`pins_ended` / `pins_dropped` / `strip_dropped`, or null); its strip holds only
cells of `ring_capture_epoch`, and a pin of another capture is listed without an
image until it is unpinned (the decoder refuses anything else; review rounds
2-3 of #749). The key is a
listing cell or a monitoring snapshot row (`MonitoringRow.start_generation` and
`capture_epoch`, new in ABI 41; centroids in ROI image coordinates); `cellKeyArgs` sends
canonical decimal strings and throws
`INVALID_U64` / `INVALID_OBJECT_ID` before anything is sent. Pin, unpin and
clear are control commands on the remote server (`VIEWER_ONLY` for a viewer).
The workspace's Experiment · Live (A6b, "Opt-in workspace" above) uses them. Guard:
`monitoringCells.test.ts`, `framePacket.test.ts` (the cell golden); for Live, `liveModel.test.ts`,
`LiveView.test.tsx` and `navigation.test.tsx`.

## Central profile registry (ABI 25, issue #398)

**Settings → Central Methods…** opens `desktop/src/CentralMethodsPanel.tsx`:
sign in/out, refresh (also on open when signed in), cancel, and the cached
revisions with each central state shown as itself. All wording and enablement
come from the pure `desktop/src/registry.ts` view model (vitest
`registry.test.ts`). The panel polls
`fetch_registry_snapshot` every 250 ms only while open and re-renders on a
generation/busy change; the password field is cleared on submit. The HTTPS
transport is `src-tauri/src/registry_transport.rs` (`ureq` + rustls with the
platform verifier; HTTPS only, no redirects, global timeout, response cap,
CR/LF header refusal, helper thread so a cancel returns at once, never panics
across the FFI), installed with `set_registry_transport` when `AppState` is
built — before the UI's `init`. Enabled by the same
`MIB_PROFILE_REGISTRY_URL` / `_PUBLISHABLE_KEY` environment as the backend
worker. The registry commands live in the desktop crate's `registry` module
(`src-tauri/src/registry.rs`), not in `mib-app-commands`: they need the shell's
transport, and a sign-in carries a password that must not cross the YOFO Studio
WebSocket. `dispatch::tests::every_command_is_dispatchable` exempts `registry::`
commands for that reason.

#398 M2b: rows are selectable and show local validation on this instrument
(`local_validation`, contract `registry_local_validation`) and the instrument
label. **Materialize** (`registry_materialize`) and **Mark validated… /
Record failed run…** (`@tauri-apps/plugin-dialog` picker →
`registry_record_validation`; the backend refusal is shown) work.
**Apply…** (#398 M2c, bridge ABI 29; materialized published/superseded rows) calls
`registry_plan_apply` and shows the changed config.json keys and the camera
script path (`applyConfirmText`); **Apply** then calls `registry_apply_method`,
which runs the backend config.json applier (`app::applyCentralMethod` →
`applyConfigDocument`, the same validated core as local profiles below; inside
the coordinator's idle transaction, refused unless the experiment is Idle and
while raw recording, capture, realtime processing or autofocus runs; an ROI with no
preview and no known camera window is pending until the first frame) and reports
applied, pending and not-applicable sections (`applyResultText`; a refusal shows the
backend's reason, which names the field and its bound). The React shell has no
config.json file: the applied method lives in the backend for the session.

#398 M3b: **Methods / Drafts** views. Methods adds **New draft** (optionally
"from current config.json"), **Approve / Reject / Publish / Archive / Revoke**
by project role with a required reason box, **History**, a details block and
"rN available"; Drafts lists local drafts with base and status, edits release
notes, submits, and on a conflict shows the compared changes with **Submit as
branch**, **New draft from head** ("keep my config.json") and **Discard**;
**New method from current config** takes a project and a name. Rules live in
`registry.ts` (`reviewActionsFor`, `draftActionsFor`, `draftRows`,
`conflictText`, `detailLines`); commands go through
`registry_new_draft_from_revision`, `registry_new_method_draft`,
`registry_set_draft_notes`, `registry_draft_from_head`,
`registry_submit_draft`, `registry_delete_draft`, `registry_transition`,
`registry_fetch_history` (each `{job_id, error}`).

## September 23 catch-up: camera setup

`desktop/src/cameraScript.tsx` wires the existing EGrabber script apply/reset
commands. Script selection and pending ownership live in App across navigation;
a native picker selects an existing `.js` file without applying it. Apply and
Reset require a stopped, configured EGrabber camera and no active experiment,
then re-read selection to reject stale capture/device state. Script text editing
is external; MindVision uses the Connect JSON path. DOM regressions cover stale
selection, duplicate commands, errors and disabled setup states.


## September 23 operator integrations (ABI 15)

`HardwareControls` stays mounted across tabs so pending command ownership and
hardware form drafts survive navigation. Connect exposes existing numeric-COM
pump/autofocus APIs; manual actuation requires the shared service-mode arm,
which clears on one action. Typed endpoints remain follow-up work.

`configDocument.tsx` owns a saved processing-document draft in App. Open/reload
are explicit; changed-field patches avoid resubmitting unknown additive keys.
Save and Apply uses a SHA256 baseline through the shared Qt-free transaction,
reports saved/applied/verified separately and preserves drafts on conflict or
partial failure. It changes image_processing only, not startup file selection,
ROI/calibration/realtime/camera settings. The old live JSON editor remains a
separate, non-persistent path. DOM tests cover conflict, partial outcomes, repeated
saves, navigation, command failures and duplicate submissions.

`exportControls.tsx` uses the shared export engine for CSV/images/all, retains
status across navigation, and exposes cancellation and partial paths. Polls are
single-flight and status errors do not erase command failures. Conversion uses
known current calibration or delegates to the backend's current factor; no
fabricated UI fallback value is sent. Chart images/series range controls remain
unexposed. Config and export controls have interaction tests, not just codec tests.


Monitoring placeholders are now bounded SVG plots over existing snapshot rows:
deformability vs raw pixel area and dimensionless ring-ratio histogram. They
cap at 200 rows, discard non-finite values, retain negative/range extrema, and
label ring ratio separately from physical ring width. No calibration, new metric
computation, or identity-matched overlay is implied.

## Software replacement follow-up (2026-09-23)

Preview pause/scrub uses exact decimal frame identities and the bounded pull
scheduler. Preview TIFF/AVI buffer export delegates to PlaybackService through
the facade, requires stopped capture and idle experiment, rejects evicted ranges
rather than silently clamping, reserves a new destination and reports retained
partial output. AVI explicitly cannot preserve per-frame timestamps. Navigation
retains the save owner. Shell status reconciliation continues independently of
capture buttons and monitoring requests are single-flight with stale-view guards.

### Hardware endpoint parity (2026-09-23)

Connect now supports typed OEABT endpoint IDs alongside CoreMOR connections and
read-only asynchronous nanopositioner discovery. Selecting a discovered endpoint only
fills the draft; connection still requires an explicit command. Incomplete/ambiguous
results are never auto-adopted. The acquisition pulse generator has system-port and
Modbus-address selection, exact-address scoped discovery, channel frequency/duty and
output enable/disable controls. Writes consume the shared commissioning arm; mounting,
polling and discovery do not write. Disconnect does not stop a generator's physical
pulse output, and the UI explains that distinction. Hardware acceptance remains a
separate deferred bench run.

Native workflow verification uses Tauri's supported tauri-driver/WebKitWebDriver
protocol with embedded production assets (`custom-protocol` Cargo feature).
A development-URL process-alive launch is not sufficient to verify the UI loads.
The desktop-shell environment section includes the native WebDriver package.

### Local profiles and checked activation (2026-09-23)

The Experiment configuration page now hosts an App-owned local profile draft/library
(`desktop/src/profiles.tsx`). Choose a Qt-compatible profiles directory; import a JSON
document, preserve/edit optional `egrabberConfig.js`, save as new, duplicate, rename,
or recoverably archive. Unknown config bytes survive copies. Mutations use a baseline
hash over config, optional script and metadata. Existing names are never overwritten.
New Draft from Current Config copies the complete document loaded in the App config
editor; without a loaded document, open config.json there or import a config to draft.
Choosing a folder preserves the draft without a discard prompt. Empty or invalid drafts are refused by both the
save button (with a reason tooltip) and the save handler. Reading saved profiles
continues to preserve their complete JSON and optional script.
Navigation retains drafts and pending commands; experiment-active operations are refused.

`BackendFacade::profileCommand` owns the portable `app/ProfileStore` path. Apply goes through
the one validated applier it shares with central methods (`app::stageConfigDocument` /
`commitStagedConfig` in `ConfigDocumentApply.h`; the profile records a provenance
document, a central method its exact text). It validates all supported settings before
changing stopped runtime services (refused during raw recording too): processing, buffers,
realtime batches/mode, frame delivery, calibration, autofocus configuration, and ROI
(bounded to the latest preview frame, else the camera window, else pending until the first
frame, with Start waiting). No camera script is executed, device connected,
or voltage actuated. Processing-contract metadata incompatibility fails closed; declared app-version bounds are checked against the compiled application version. The saved processing editor remains
a separate checked persistence workflow and can open the selected profile's config.

Remaining shell integration: profile ID propagation into experiment requests and
display-FPS presentation hookup. Remote/startup workflows are described below.
The profile apply reply returns `profile_id`/`display_fps` for those shell integrations;
these are not falsely reported as backend-applied settings. Directory publication is
atomic within the filesystem; revisions serialize this backend, not external Qt writes.

Native webview verification exposed a permanently disabled Start Experiment
button left from pre-ABI-13 scaffolding. Start now selects an output path, fetches
authoritative backend readiness, displays blocking gates, and invokes the shared
backend Start transaction (which rechecks readiness). A synchronous pending owner
prevents duplicate chooser/start requests; Starting joins Active/Stopping guards.

Startup discovery is now available through an explicit auto-select/retry panel and a
persisted opt-in preference for subsequent startup sessions (off by default). Scheduling
acceptance is distinct from camera-configured/nanopositioner-connected completion.
`onSelectionChanged` refreshes shell-owned camera state when startup results change it.

The Tauri facade installs a queued startup executor: discovery workers enqueue bounded
job callbacks, and serialized status polling drains them on the bridge caller. This
matches Qt's UI executor ownership instead of mutating camera selection on a provider
worker. AppBackend rechecks idle experiment/capture state at actual selection/connection,
not merely when the scan starts. Empty/ambiguous/incomplete results retain manual choice.

Review Close File now calls the shared facade rather than a disabled placeholder.
The facade rejects active recording/experiment and clears cached source identity;
the shell clears review pixels/metrics only after successful closure. Startup
selection completion refreshes the camera selection in the main shell.
Since #512 S5 the facade's loaded review file is its own `ReviewSession`
([[../services/ReviewSession]]), not the experiment writer's handle; the
Review tab's commands and shapes are unchanged, and a loaded file still keeps
Start Experiment and Record refused (readiness `storage.hdf5`, same text,
decision D1 (a)) until Close File, as before; since F2 of #763 the gate and
the refusals say "Close the file in Review (Review → Close File)".

### Finite background calibration

`BackgroundCalibrationControls` exposes the shared realtime calibration operation:
required empty frames, maximum examined frames, finite timeout, progress/rejection
counts, cancellation and published generation/SHA-256. An experiment must be Idle to
start; cancellation remains possible later. Scheduling success is not publication,
and a failed/cancelled candidate never replaces the previous background. Mount this
component with `onPublished` refreshing processing/background state.

### Native acceptance gate

`desktop/scripts/native-workflow.py` runs the embedded-assets application under
Tauri WebDriver + Xvfb, configures mock frames using React controls, uses actual
GTK file dialogs (xdotool), starts an experiment, navigates away, stops/finalizes,
reopens HDF5, exports through the shared service and closes review. Backend IPC
is never mocked. It requires nonzero conserved persistence and published export
outputs, saves screenshot/status evidence, isolates app data and cleans up its
owned session. Desktop CI now runs this in addition to unit tests and launch smoke.
Initial local pass: 2 persistence-admitted/committed frames, 2 exported images.
The gate exposed and drove fixes for the disabled Start button and missing
realtime processing consumer. It does not establish physical hardware timing.

### Managed profiles and startup provenance

`profile_command` now also supports `selection`, `restore` and `install_remote`.
A successful explicit apply persists a small `.selection.json` pointer in the selected
profiles folder; the App-owned hook restores it once after backend readiness, only when
the config/script/metadata aggregate revision still matches. No scripts, device connects
or hardware actuation are performed by restore. `selection` separately returns the saved
startup choice and actual runtime `profile_selection` provenance; a saved choice alone
is never evidence of application. External edits fail closed for explicit review/reapply.

**One startup pointer (#398 M2c, bridge ABI 32).** Since central methods can be applied
too, the startup choice is the last thing applied, whichever kind: `app/StartupConfiguration`
keeps `<dataDir>/startup_configuration.json` (`{"kind":"profile","base","name","revision"}`
or `{"kind":"central","revision_id","config_sha256"}`). The profile store's `apply` and
`app::applyCentralMethod` both record it. At startup the profiles hook calls
`restore_startup_configuration(profileBase)` once, even with no profiles folder chosen:
- a profile goes through the profile store's `restore` in its recorded folder;
- a central revision is re-planned from the registry cache (reopened offline) and must
  still match the recorded sha256;
- with no pointer, a legacy `.selection.json` in the chosen folder is still restored.
A failed restore applies nothing and returns a `notice` (the instrument runs on its default
settings until a method or profile is applied), which the panel shows. The pointer stays,
so the notice repeats at the next start until then. An ROI with no frame yet is pending,
never a failure. `.selection.json` is still written for the folder's own selection.

Catalog transport is bounded to 4 MiB/HTTP(S), has finite timeouts, no redirects or URL
credentials, and runs outside the backend bridge mutex in Tauri's blocking pool.
`profileCatalog.tsx` provides passive catalog checks, full config-field and camera-script
diffs, explicit install/update and local-name selection. Backend installation verifies
required SHA256 checksums, app version bounds and active processing-contract compatibility
before publishing; updates require the existing revision/profile identity and preserve
the complete old directory under a hidden `.backup-*` path. An update never changes runtime
settings, and an obsolete startup pointer consequently requires an explicit apply.

### Packaged resource roots

The Tauri shell passes its resolved read-only resource directory separately from the
writable application data directory through `initialize_with_resources`. Bundles include
the default configuration and isoelastic LUT resources; backend model/LUT lookup no
longer assumes the application data directory is beside the executable. Legacy Qt
initialization retains its existing data-parent fallback.
### Atomic processed overlays

`ProcessedPreview` is a separate coherent processed-frame viewer with ROI, mask,
contours and explicitly primary-object target overlays. It fetches one bounded binary
`MIPO` v1 envelope containing metadata plus grayscale source and mask bytes. It never
pairs an independently fetched latest frame with independently fetched analytics.
At most 32 MiB per image and 20,000 contour points/512 contours are delivered; contour
truncation is displayed. Unknown timestamp units remain unknown. UI polls at 5 Hz with
one request in flight, enables retention only while active and disables it on cleanup.
Pass `ready` and `active` from the owning Preview page; only mount one consumer.
### Core and application release controls

`coreManagement.tsx` retains operations across navigation. Core restoration completes
before profile restoration; `cores.initialized` is the shell startup gate. Reopening a
webview during an active experiment reads status rather than switching the kernel.
`processing_core_command` runs expensive signature/cache verification in Tauri's blocking
pool and delegates lifecycle authorization to the facade. The native test covers bundled
roundtrip, corrupt persisted selection, failure recovery, persistence faults, unsupported
signature schemes and concurrent activation requests. See [[frontend/ProcessingCoreDialog]].

Settings → Updates now reaches these controls and read-only application release checks.
The existing Rust manifest verifier recognizes both its `url`/`sha256` names and Qt's
published `installer_url`/`installer_sha256` names. It does not launch Qt installers as
Tauri updates. Tauri-specific package publication/installer launch/rollback remain release
work; a successful manifest check is not proof of installable Tauri delivery.

### Integrated close and delivery checks

File→Exit and native window-close share an authoritative close guard. Pending saves,
exports/reanalysis/calibration, raw recording and nonterminal experiments postpone
closing; configuration/profile drafts require explicit discard. Idle capture stops
before close. Closing never implicitly cancels a scientific run. Settings and Help
menus route to implemented controls and the issue page rather than disabled stubs.
The integrated bridge ABI is 17 (processed/source review packets and core management).

`desktop/scripts/bundle-deb.py` packages an already-built custom-protocol binary,
deriving native Debian dependencies with `dpkg-shlibdeps` (including OpenCV/HDF5,
not just GTK/WebKit). Use `--debug` for development verification; release packaging
requires the release binary. Build on each supported target distro; a local package
is not evidence of Windows/macOS delivery or signed automatic update acceptance.
Live processing config refreshes preserve dirty drafts and detect changed runtime state;
explicit reload cannot overwrite edits entered during a pending fetch. JSON submissions
validate all fields before mutation and reject stale config_version snapshots. Profile
selection provenance includes display_fps, which controls the live frame request cadence.

The native acceptance harness additionally requests File→Exit during an active run
and refreshes the entire webview, requiring the same native experiment/output to
remain active. Shell statistics polling is independent of a possibly stale UI draft
of the realtime-enabled toggle. Raw MIBF v2 acquisition/store epochs require bridge
ABI 18; processed preview recipe identity remains separately scoped.

## PZ7035 instrument surfaces (#501 P0a, 2026-10-05)

The UI follows `fetch_platform_info` `capabilities` (`platformCapabilities.ts`;
a server without them is the MIB desktop). On the PZ7035:

- **Hidden:** host background, autofocus/nanopositioner (sidebar and
  Hardware), framegrabbers and the EGrabber camera script, MindVision, the live
  frame buffer, core updates, HDF reanalysis and the pulse generator.
- **Pumps:** default to the instrument's two peristaltic pumps on
  `/dev/ttyPS1`: Sample at Modbus address 3, Sheath at 4, 25 µL/rev
  (confirmed on the bench 2026-10-05). Infuse turns the heads
  counter-clockwise.
- **Sidebar:** gains "PL core": build, weights, LED and latency max.
- **Preflight** (`preflight.ts`) replaces the host core pin with:
  - **PL core** (build vs `expected-core.json`, weights vs the pin);
  - **Sensor link**: warn above 10 ingress errors/s or 1 resync/s;
  - **LED strobe**: a guard trip fails.

  Autofocus and trigger read "not on this instrument". The quality gates
  keep only calibration until the image focus metric (P0b). A healthy
  instrument at idle shows 0 warnings (`preflightPz7035.test.ts`).
- **Token prompt** (`components/AuthGate.tsx`, `transport/auth.ts`): in a
  browser the app mounts only after `GET /auth` accepts the token, taken
  from `?token=` or typed once into sessionStorage. Otherwise it shows a
  prompt or "unreachable". The socket URL is read when it opens, so a typed
  token counts.

## PZ7035 Align and Run (#501 P1, 2026-10-05)

With `capabilities.align_mode` and `run_mode` set, tab changes drive the
backend's camera modes instead of `set_camera_overview`:

- **Opening a tab switches the mode.** Camera & Alignment means Align: the
  full sensor at 400 fps, shown as whole frames from the PL bridge (results8
  on, LED 100/135 µs) or as the producer's bands on older images (LED
  0/125 µs). Status `mode.align_source` tells which. Align checks that a preview arrives: if
  the ingress is stuck (P[13] bits 15:8, the sticky lane overflow flags, every
  frame lost; #629) it makes up to eight 100 ms receiver resets (`AlignLock`,
  counted in `mode.align_lock`) before the operator error "Align preview not
  locking" (on results9 the PL self-heal block does the resets and the host
  only waits, `link.rx_heal`). With no client for the server's grace time the instrument goes
  idle (LED off, cell path off, camera released) and a reconnecting page
  resumes the tab's mode. Experiment means Run at the
  window placed there: 512×96, x on 8 and y on 4 (`snapRunWindow`), LED
  7/60 µs, the U-Net on.
- **Placing the window.** Dragging only moves it; releasing (or Save camera
  ROI) places it, and the switch to Run applies it. State and handlers live in
  `cameraWindow.ts` (`useCameraWindow`). A window shown only as the default
  (the sensor centre) is not placed. After a page reload the UI takes the
  window the backend applied from `fetch_instrument_status.mode`
  (`run_set` true once a Run switch succeeded in the process, then `run_x`,
  `run_y`; `restoredRunWindow`), unless the operator already placed one. The
  backend also writes it to `<data dir>/instrument_run_window.json` at every
  successful Run switch and reads it at start-up (`AppBackend::loadInstrumentRunWindow`),
  so an instrument restart keeps the window; a file that is not on the Run grid
  or the sensor is ignored, and the window is only ever replaced, not cleared.
- **When the camera cannot go to Run.** `components/RunWindowNotice` shows an
  alert in the tab, like a failed preflight check: with no placed window the
  Experiment tab says "Place the 512×96 run window in Camera & Alignment" and
  offers a button to go there (the tab switch does nothing until it is placed);
  a refused Align/Run switch shows the backend's reason with a Retry button
  (`modeError`). Start Experiment is disabled with the same reason
  (`runModeBlockReason`). On the PZ7035 Start no longer asks for a running
  camera stream, since Run stops the producer; the backend's `instrument.mode`
  gate is the check.
- **Other cameras.** Without `align_mode`/`run_mode` the window is the camera
  ROI: released drags are snapped and saved with `save_camera_roi`
  (`cameraWindow.test.tsx`).
- **Run preview.** In Run the live camera is stopped. The Experiment preview
  polls `fetch_run_preview` every 100 ms (`runPreview.ts`) and draws the PL
  gray frame with the U-Net mask tinted (toggle) and the listed cells' boxes
  (valid green, invalid red). A status line shows the frame, listed cells,
  cells, blemishes and the latency max.
- **Service mode.** It also sets the backend latch (`set_service_mode`). In
  it, `InstrumentLedControls` adjusts delay and width (±0.5 µs steps,
  clamped to the limits) or restores the preset, and shows a read-only readout of the strobe
  control S[0], the guard and its trip count (an alert on a fault). The next mode switch
  restores the preset anyway.

**Pump rate in rpm (#501).** For a Tushui peristaltic pump the Rate unit select also offers "rpm
(head)" (`pumpRate.ts`, pseudo unit 200): the UI sends flow = rpm x the calibration the pump was
connected with (µL/rev), in µL/min, since the backend takes a flow; it refuses an rpm without a usable
calibration. The panel shows the configured flow as "≈ x rpm" and labels the direction "Infuse (CCW)" /
"Withdraw (CW)" (the Tushui heads). Starting, purging and changing settings still need Service mode
and one-shot arming, and nothing is sent at load; stop is always available.

**Link health, sensor and latency (#501).** The PL core sidebar section shows three more rows from
`fetch_instrument_status`: **Sensor** (the ingress geometry from S[29] at the actual frame rate from
the XVS period in S[9], 100 MHz clocks; "closed" while no XVS runs; the run window's x is a multiple of 8
and y of 4, as the producer's offset check requires, and the Camera & Alignment hint says so), **Link** (P[12] errors, P[14]
resyncs, P[7] bad and P[6] dropped frames per second; only resyncs above 1/s and the 5 s average of errors above 10/s warn (`link.ingress_errors_warn`, `ingress_errors_avg_per_s`: Run at 5 kHz shows bursts of 8–13 errors, about 1.1/s on average, so one burst must not warn),
bad frames above 1% and dropped frames above 0.1% of the sensor's frame rate warn only when sustained for 5 s
(`link.bad_frames_warn`/`dropped_warn`, from the backend; a drop in Run is data loss), so a baseline of a few per second
does not (about 0.1/s at rest)). Dropped frames are judged only while the results bridge is ARMED or RUNNING
(`link.bridge_active`): P[6] counts every frame as dropped while nothing consumes the bridge (Run without an
experiment, a stopped Align), which is not loss and **Latency** (S[47–51]: max in µs and frames over budget of the frames seen). The
preflight Sensor link check lists the same rates and the sensor. The backend reports the rates
invalid (`link.rates_valid` false, "measuring…") for 1.5 s after a camera mode switch
(`PzPlatformMonitor::settle`, called by `AppBackend::setInstrumentMode`), because the receiver and
sensor reset then and the counters jump; the first rates afterwards are measured from inside that
window, so the switch's spike never shows. Pure formatting in `linkHealth.ts`.

**Recording to RAM (#501).** `fetch_instrument_status.storage.warning` feeds
three places:
- the preflight Storage check (a warning, not a failure);
- the context bar's Storage segment ("RAM");
- a status note after Start Experiment, from the readiness gate
  `storage.persistent`.

None of them blocks a run. Desktop builds are unchanged.

## How a run ended (#549)

`desktop/src/runOutcome.ts` (pure) turns the finished run's experiment status
(`completion`, `completion_reason`, `persistence_admitted`) into an operator message: the
completion, the non-zero loss counts parsed from the backend's reason
(`storeOverwritten`, `storeNotCommitted`, `storeMalformed`, `processingFailed`, `sequenceGaps`),
and the lost fraction of the admitted frames. A booked malformed frame (an ingress error) is a
declared loss: the run is "partial (declared)" and the notice is informational unless malformed
frames exceed 0.1 % of the admitted frames (`MALFORMED_WARN_FRACTION`). `components/RunOutcomeNotice`
shows it on the Experiment tab (an alert for undeclared loss, failure, an unknown outcome or that
attention case, a status otherwise), with the backend's own text as the tooltip. The shell logs one line per finished run and adds a "Last run"
row to the sidebar. Cancelled runs and a status that is not yet terminal show nothing.

The reconciled accounting (`fetch_run_accounting`, ABI 31) supplies the denominator: the frames
the run admitted, not the rows it saved (a run with many EMPTY frames saves far fewer rows than it
admits). `describeRunOutcome(status, accounting)` uses it when the accounting belongs to the same run
(`start_generation`); `describeReviewOutcome(accounting)` describes the file loaded for review. The
Review tab shows it above the export options: a raw recording or a legacy file without accounting is a
quiet "no run accounting saved" note, and a file whose counters do not reconcile reads as a failure.
Tests: `runOutcome.test.ts`, `RunOutcomeNotice.test.tsx`.

## PL mode, branding and reload (#550 m11–m15)

- **Capability gating.** In PL mode (`hostProcessing` false) the legend, Clear ROI, manual ROI
  fields, the host core line and the host rates in the status bar are hidden; Clear Background and
  Auto background follow `caps.host_background`; the EGrabber script checkbox in the profiles panel
  follows `caps.egrabber_script` (the `egrabberScript` prop of `ProfilesPanel`).
- **Name.** `brand.ts` `productName({pz7035, remote})` is YOFO Studio on the instrument and in the
  remote browser UI, MIB Studio on the desktop. The remote title is set in `main.tsx` before the
  first render; `App` also sets `document.title` and the About heading.
- **Reload.** `persistedState.ts` mirrors the event log and the two stage confirmations to
  sessionStorage (per tab, guarded against missing storage). The workflow still compares a restored
  confirmation with the current signature.
- **Narrow windows.** Below 1100 px the tab labels and camera buttons do not wrap, the camera buttons
  move below the tabs, and the status line wraps.

**Align focus and brightness (#501).** There is no nanopositioner ring ratio on the PZ7035,
so the operator focuses by hand against a number from the live image (`imageQuality.ts`,
pure and unit tested):
- **Focus number.** The variance of the 4-neighbour Laplacian over the interior of the Run
  window, the 512×96 box the U-Net will see. It rises toward best focus. The window's best
  value is held and restarts when the window moves, the mode changes, or the operator presses
  "Restart focus peak" (after changing the sample). On a real lit 512×96 frame it falls from
  339 to 1.5 as a box blur grows from radius 0 to 8.
- **Brightness.** Mean, 99th percentile and the saturated fraction of the same box. The gate
  warns below 40 DN (dark; a lit Align frame is about 143 DN, a dark one about 18) and above 1 %
  saturated.
- **Where it runs.** `draw()` measures each displayed whole frame (about 50 k pixels) and
  publishes at most every 200 ms. It only measures an 816×624 Align frame while a window is
  placed. Run has no live camera frame, so the gates read "unknown" there.
- **Gates.** `quality.ts` adds Focus and Brightness for the PZ7035 only when the Align view
  supplies an image. The focus gate has no absolute pass level, because it depends on the
  sample; it reports the number and its share of the best seen.

## Pump model per slot (2026-10-04)

The Pumps panel (`HardwareControls.tsx`) has a **Pump model** select per slot:
Syringe (Longer dLSP) or Peristaltic (Tushui). Peristaltic adds a calibration
field (µL per revolution, default 25), pre-fills the instrument endpoint
`/dev/ttyPS1` address 3 when the fields are untouched, hides the syringe
volume controls and shows head rpm and the estimated delivered volume.
Connect goes through `pump_connect_model` (ABI 22, [[Rust-Bridge]]), which the
WebSocket server allows for the controlling client.

## Remembered discovery and named pump endpoints (2026-09-23)

Startup selection now installs validated, per-user remembered vendor/endpoint/baud/address preferences into the shared startup coordinator before optional automatic selection. Malformed persistence skips automatic selection; failed persistence is distinguished from a session-only applied preference. Preference changes do not connect hardware.

Pump connections accept system serial names (including Linux paths), reusing the existing shared SerialBus string transport. Status exposes the actual port name; legacy Qt config edits preserve connected transport identity. Two pumps can share a bus at distinct slave addresses, while duplicate pump/pulse slave identities and autofocus port collisions are refused before connection writes. Legacy numeric COM bridge calls remain supported. Native fake-serial tests cover named endpoint roundtrip and shared-bus identity guards; real hardware acceptance remains deferred.

CI also builds and extracts the Linux development `.deb` and runs the same native
workflow from its packaged path, retaining package and evidence artifacts. File-dialog
acceptance waits for actual GTK dialog closure: a slower hosted runner exposed that
a fixed 300 ms folder-navigation delay could leave the export chooser pending.
Webview reload and native process startup are distinct: `is_initialized` selects a
read-only reconciliation path for retained sessions. Runtime flags, experiment status
and review metadata must all resolve before readiness permits startup hooks. A reload
never reapplies saved profile/core selections over current native state. Existing export
and reanalysis jobs are polled by App-owned hooks; unknown initial status is busy, not idle.

Monitoring scientific scope: `MonitoringRow.area` is raw px²; `youngs_modulus` is the
stored kernel/LUT kPa result (zero means unavailable). Tauri renders a bounded raw-area
scatter plus valid-object ring-ratio and modulus histograms. It does not apply current
calibration to retained historical rows. Live calibrated scatter/isoelastic parity needs
per-row calibration provenance captured by every inline and batch processing path; the
current Qt live scatter's use of the current factor is not authoritative for mixed epochs.
## Windows nonpublishing candidate lane

`.github/workflows/desktop-windows-candidate.yml` builds a Windows x64 SDK-free Tauri candidate on `dev/react-tauri` pushes or manual dispatch. This is separate from the existing Qt Windows release workflow and never creates tags, releases, update feeds or signed installers. It uses the repository VS2022/MSVC194 Conan profile, VS CMake backend-only build and existing bridge link-manifest generator, then release-mode Rust tests/build.

`desktop/scripts/package-windows-candidate.ps1` creates a fresh portable directory and ZIP: recursive non-system native DLL dependencies (unresolved/conflicting names fail), app-local VC runtime, defaults, isoelastic LUT resources. The staged application is smoke-launched with development DLL search paths removed. WebView2 Evergreen remains an explicit prerequisite. The candidate disables EGrabber, MindVision and CoreMOR SDKs; SDK-enabled camera delivery and Windows hardware acceptance remain separate gates. Windows hosted execution is required before declaring this candidate validated.

Local minimum path: VS2022 x64 developer PowerShell, Node22, stable Rust/MSVC, Python/Conan/CMake; install dependencies with `conan install . -of build --build=missing -s build_type=Release -pr conan/profiles/windows-msvc194`, provision required assets, configure `windows-default` with the workflow's SDK-free/backend-only flags, build backend libraries and `mib_backend_smoke_test`, run `tools/gen_bridge_link_manifest.py`, then `npm --prefix desktop ci`, frontend test/build and release Cargo desktop build with `custom-protocol`. Run the packaging script last. Never use Qt's release workflow to build this candidate.

The close guard also queries native initialization while shell boot/recovery is still
pending; a temporarily false React `ready` flag cannot bypass protection for an
existing run. A refused close reveals the log so the reason is visible immediately.
stored kernel/LUT kPa result (zero means unavailable). The host stamps each FilterResult
with the exact pixel-to-micron factor passed to kernel object analysis, across inline and
batch paths. Tauri and Qt live scatter use this per-row factor, never current calibration;
unknown-factor rows are omitted from calibrated scatter. Tauri also renders valid-object
ring-ratio and modulus histograms. Optional isoelastic reference curves reuse the bundled
Qt/review resource and label channel/flow/viscosity conditions, not inferred hardware state.
The calibration stamp is host-only, not a ProcessingCore C ABI or persisted HDF change.

## Explicit operator recovery (Tauri)

A persistent recovery panel displays failed experiment output path, exact committed/admitted/failed counters and terminal error. Matching Qt's explicit acknowledgment, the operator must review a checkbox before clearing the readiness fault. The coordinator compares the displayed run generation, monotonic fault occurrence revision, fault code and message under its lifecycle mutex and refuses active/flushing/replaced or already-cleared faults. Acknowledgment does not alter the recorded failed outcome, repair/delete its file or automatically restart. Start readiness is still evaluated again.

Capture lifecycle polling exposes exact capture/failure generations, retained failure details and authoritative camera-ready state. Explicit retry reuses the existing configured-camera Start path; it is blocked during experiment finalization/recording, never retries automatically, and does not equate accepted start with a ready device. Configuration review remains available. These panels persist across stage tabs; no hardware execution is required by their tests.

ABI 19 adds analysis-time monitoring calibration/reference curves and explicit
fault-revision recovery/capture-lifecycle status. The full non-network/non-hardware
115-test CTest selection now passes (one optional exporter soak skipped) after
installing declared Python build requirements into an isolated verification venv.
Hosted folder chooser acceptance clicks GTK's Open button while preserving the
typed location and asserts the exact exported destination; failures also capture the full X11 desktop for diagnosis.
Windows candidate runs are not cancelled mid-Conan build by each branch push; the
latest queued candidate can reuse the completed dependency cache.
### User-operated application installers

The application updater reuses Qt's HTTPS manifest and SHA256 trust model; it is not a
second signing authority. `check_tauri_app_update` uses only
`https://updates.yofo.bio/{stable|beta}/tauri/{windows|linux}-{x86_64|aarch64}/latest.json`.
The manifest must contain `version` (newer SemVer than native app_version), `installer_url`
(HTTPS, no credentials), `installer_sha256`, `installer_size_bytes` (1..4 GiB), matching
`channel`, `artifact_family: "tauri"`, `os`, and `arch`. Qt/unidentified artifacts are rejected.
Supported package launch formats are Windows EXE/MSI and Linux DEB/RPM through the native
opener/package installer; AppImage/macOS installation is not claimed.

Browser download is followed by operator file selection, bounded streaming SHA256 copying
into a private cache directory, then separate explicit launch confirmation. Native tickets
expire after 15 minutes; a replaced ticket, changed manifest, wrong platform/version,
size/digest mismatch or changed staged bytes fails closed. Both verification and launch
re-fetch the canonical manifest. Capture, recording, experiment finalization, export,
reanalysis, calibration, pending UI work and unsaved drafts must be inactive. Launch holds
the native command mutex across the final idle check and opener request. The app never
automatically exits; opener acceptance is not reported as completed installation. Feed
publication, signing infrastructure and real-installer acceptance remain release tasks.

### Qt-free Conan graph

The root Conan recipe defaults `with_qt=True` to preserve Qt builds. The Windows Tauri candidate explicitly supplies `-o '&:with_qt=False'`; backend libraries retain all existing version pins, while Qt and its Linux-only xkbcommon/Wayland overrides are omitted. `tools/test_conan_recipe.py`, run using the Python environment containing Conan 2, verifies the default and disabled graphs' direct requirements without network access. Full dependency resolution still needs the pinned recipes/binaries in cache or configured remotes.

### Portable camera-document Save As

Camera editors stage bounded validated content in a randomized `tempfile::NamedTempFile` in the destination directory, sync file data, then use `persist_noclobber` for Save As. The maintained tempfile implementation uses non-replacing `MoveFileExW` on Windows (including filesystems without hard links) and native no-replace rename where supported on Unix; unavailable safe publication remains an error rather than an overwrite fallback. Existing Save retains its revision recheck and file permissions before replacement. RAII removes staging files on failure. Tests cover exact Unicode/revision roundtrip, existing file/directory conflicts, concurrent creators, bounded input and legacy staging-name collisions. Removable-media hardware/mount testing is not claimed.

## Z stage panel (#464, slice 5)

`StageControls` (`desktop/src/components/StageControls.tsx`, pure rules in
`stageControlModel.ts`) sits under the pump and autofocus panel on the
Connect tab. It uses only the `stage_*` commands and `fetch_stage_status` from
[[Rust-Bridge]] (ABI 30). It is hidden on the
PZ7035 until that board has a serial path for the stage. The service rules are
in [[../services/StageService]].

**The panel mirrors the backend rules; it does not replace them.** A disabled
button is a courtesy, not a safety gate, and the reason for every disabled
control is shown.
- **There is no Home** (ADR 0013 Amendment 1, ABI 30). The stage is never
  homed; the panel offers "Set zero here" instead.
- **Position is shown as unknown until zero is set**, together with the
  controller counter. The counter is only a position once the operator has
  set zero, and even then a hand move or stall is invisible.
- **Moves need the zero to be set this power-up.**
  - Targets are whole micrometres.
  - They are pre-checked against the travel envelope the backend reports
    (±1000 µm around the zero, ±2900 µm after a mid-travel declaration); the
    backend enforces it, refusing instead of clamping.
- **Set zero here…** asks for confirmation. The warning says it moves nothing
  and that nothing checks where the stage physically is; an optional
  "the stage is at mid-travel" checkbox widens the travel and does not carry
  over to the next zero. It needs Service mode and arming like the pumps.
- **Session-only zero warning:** when the backend reports `session_only_zero`
  (hardware-acceptance mode, power-cycle detection off) the panel shows an
  alert; it is silent otherwise.
- **The limit switches are a badge, not a gate:** the indicators carry
  "wiring unverified" until a supervised `zc300ctl verify-limits` passed, and
  that never enables or widens anything. The home bit is not shown (it floats).
- **Arming is one-shot**, consumed only when a move or Set zero is actually
  sent. A rejected input (a mistyped target, a target outside the envelope)
  keeps the arming.
- **Stop is always enabled and fires while the backend is ready.**
  - It does not use the panel's command lock, so a pending command cannot
    hold it.
  - It works with unreadable status, in operator mode and during an
    experiment.
- **Everything else is locked while an experiment is active.**
  Disconnect is allowed while a move runs: the backend stops the axis first.
  Apply stage settings is not, and is refused at once with `Busy`.
- **Status refresh** is 1 s, or 250 ms while something moves.
- **Endpoint discovery** needs an explicit serial port and looks for kind
  `MotionStage` with an address scope. Selecting a result fills the fields
  and never connects.

**Open follow-up:** relax the one-shot arming for small jogs only after the
first supervised session. The candidate is a jog-only arm window (about 30 s,
small whole-micrometre steps inside the envelope, any other action or Stop
disarms; #521).

Tests: `stageControlModel.test.ts` (rules) and `StageControls.test.tsx`
(panel behaviour with a mocked bridge).

Background capture/clear and ROI editing are disabled during Starting, Active and
Stopping. Setup command refusals refresh backend ROI/background state (#542).
### Metric rendering and panel recovery (#540, #543)

`metricFormat.ts` renders absent/non-finite metrics as an em dash. Both Studio
result tables share `ResultMetricCells`; review columns, frame viewers and chart
labels use the same formatter. `PanelErrorBoundary` surrounds the main panel
content in Studio and YOFO Review; Reload view remounts the failed children while
shell controls and logs stay available. Studio subtab bodies do not shrink below
content height: the outer `.tab-body` scrolls the Preflight and App-config content.
JSDOM has no flex layout engine; physical layout needs browser verification.

Safety confirmations go through `desktop/src/transport/dialogs.ts` and must be
awaited. Tauri uses the public dialog API (`dialog:allow-message`); browser
confirmation and native dialog errors fail closed unless the result is true.

Help ▸ What's New and User Manual use `OfflineHelp.tsx`. Vite eager raw imports
bundle `docs/release-notes/v*.md` and `docs/manual/*.md`; URL imports bundle manual
images. Local manual links navigate within the dialog, with an Index button and
an explicit online-documentation button. Version discovery uses Tauri's app API,
with the package version for development. This adds no bridge ABI surface.

Issue #570: Review keeps unreadable run accounting as Unknown, displaying the
existing `completion_reason` field rather than promoting `reconciled=false` to
Failed when the outcome is Unknown. Known outcomes with unreconciled counters
still show Failed. No bridge ABI or JSON field changes.
