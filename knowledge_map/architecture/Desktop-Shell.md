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
- `desktop/src-tauri/` — the Tauri v2 app. `src/lib.rs` holds `AppState`
  (`Mutex<UniquePtr<BackendBridge>>` + a cached last-frame buffer) and the
  `#[tauri::command]` layer; `main.rs` calls `run()`.
- `desktop/scripts/xvfb-smoke.sh` — headless GUI smoke launcher.
- `desktop/src/workflow.ts` — pure guided-workflow stage derivation (UX-1),
  with `desktop/src/workflow.test.ts` vitest coverage.
- `desktop/src/preflight.ts` — pure hardware-preflight checklist derivation
  (UX-3), with `desktop/src/preflight.test.ts` vitest coverage.
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

## Command layer

Thin wrappers over the bridge (all take the managed `AppState`):

- **Live capture:** existing lifecycle commands remain serialized through
  `AppState.bridge`. `fetch_frame_packet` returns one owned binary response.
- **Recording/review:** `fetch_indexed_frame_packet(frame_index)` and
  `fetch_review_frame_packet(dataset,index)` accept canonical decimal-string
  indices. `fetch_background_packet` uses the same codec.
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
  (Tauri's `frontendDist`). `tsc` typechecks under strict mode.
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

## Analysis-only development context (#399, bridge ABI 14)

Launch with `--analysis-only` or Cargo feature `analysis-only`; native startup
uses the new bridge `initialize_analysis` entrypoint. The webview cannot change
the mode. Review is the starting workspace and instrument navigation/service
mode controls are unavailable. Unqualified standalone exports stay disabled.
Raw-frame browsing uses HDF5 dataset offsets, independent of the live ring.
Persisted completion/accounting is displayed; absent raw scientific metrics are
shown as unavailable. This is not yet the one-installer product: bundled toolkit,
helper lifecycle and Windows/Linux/NAS qualification remain open in the
[execution plan](../../docs/exec-plans/active/2026-09-10-local-analysis.md).

The experimental `desktop/analysis/helper.py` endpoint is tested separately over
private framed pipes; `desktop/analysis/README.md` specifies protocol 0.1 and its
bounded histogram/KDE **page** methods. It uses Toolkit, not rewritten science.
It is not launched by Tauri and reports production readiness false. Version-only
handshake is development validation, not bundle trust. Native operation ownership,
cancellation, signed distribution identity and parent-exit handling must land before
the desktop advertises helper-backed analysis.
