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
device+core *signature* still matches — detection alone never completes a
stage, and changing the device invalidates the confirmation. The shell renders
status on each tab (text + dot, never colour alone) and a "Next" action banner,
and lands startup on the earliest incomplete stage. Unit-tested in
`workflow.test.ts` (`npm test`, gated in Desktop CI). Detailed per-stage content
is the rest of epic #304 (UX-2…UX-11). Details:
`knowledge_map/task/2026-07-21-ux1-guided-workflow.md`.

**Hardware preflight (UX-3, issue #307):** `desktop/src/preflight.ts` —
`derivePreflight(input, requirements)` builds the Preflight stage's checklist
(camera, processing-core/trust, capture stream, autofocus, sample/sheath pumps,
trigger, storage), each a `passed`/`warning`/`failed`/`not-required` status with
requirement, expected-vs-detected identity, cause, and recovery actions;
`criticalPassed` gates on all `required` checks. Required/optional/n-a per device
is meant to come from the selected profile (not bridged yet → `DEFAULT_REQUIREMENTS`
until UX-2 #306). The shell polls camera/core/autofocus/pumps/trigger every 1.5 s
while the Preflight tab is shown (off the capture loop) and renders the checklist
in the Connect tab. Storage writability/free-space stays informational until a
backend status contract exists. Details:
`knowledge_map/task/2026-07-21-ux3-hardware-preflight.md`.

**Required checks gate the workflow (#548).** `WorkflowFacts.requiredFailures` carries the
checklist's `required` checks that are not `passed` ("Label: detail"). When present it is the
authority for the Preflight stage and replaces the host core pin test, because the checklist
already holds the host core check on the desktop and the PL core check on the PZ7035. Each such
check is a blocking reason, so the stage is "needs attention", the recommended action navigates
instead of offering the confirmation, and a stage confirmed earlier goes back to "needs
attention" if one starts failing. A required check in `warning` blocks as well (the checklist's
`criticalPassed` rule). Tests: `workflow.test.ts` (#548).

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
  experiment -> `experiment_stop`, raw recording -> `stop_recording`; capture
  keeps running). The desktop close guard refuses to close instead; a remote
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
rejects calls in flight with `TRANSPORT_LOST`. `transport/dialogs` replaces the
dialog/opener plugins: native in Tauri, prompts for instrument paths and
`window.open` in a browser. In a browser the close guard only warns on
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

`desktop/dist` is served at `/` with `--dist`. Tests: `tests/ws.rs` (mock
capture over the socket, wrong token refused, client loss and quick reconnect).

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
stopping a run or claiming native cancellation. No pixels enter React state.
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
  0/125 µs). Status `mode.align_source` tells which. Experiment means Run at the
  window placed there: 512×96, x on 8 and y on 4 (`snapRunWindow`), LED
  7/60 µs, the U-Net on.
- **Placing the window.** Dragging only moves it; releasing (or Save camera
  ROI) places it, and the switch to Run applies it. State and handlers live in
  `cameraWindow.ts` (`useCameraWindow`). A window shown only as the default
  (the sensor centre) is not placed. After a page reload the UI takes the
  window the backend applied from `fetch_instrument_status.mode`
  (`run_set` true once a Run switch succeeded in the process, then `run_x`,
  `run_y`; `restoredRunWindow`), unless the operator already placed one. The
  backend keeps it in memory only, so a server restart still starts unplaced.
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
  it, `InstrumentLedControls` adjusts delay/width (±0.5 µs width steps,
  clamped to the limits) or restores the preset. The next mode switch
  restores the preset anyway.

**Link health, sensor and latency (#501).** The PL core sidebar section shows three more rows from
`fetch_instrument_status`: **Sensor** (the ingress geometry from S[29] at the actual frame rate from
the XVS period in S[9], 100 MHz clocks; "closed" while no XVS runs), **Link** (P[12] errors, P[14]
resyncs, P[7] bad and P[6] dropped frames per second, warning above 10 errors/s, 1 resync/s or any
bad frame) and **Latency** (S[47–51]: max in µs and frames over budget of the frames seen). The
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
