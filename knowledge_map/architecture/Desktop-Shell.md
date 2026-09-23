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
Navigation retains drafts and pending commands; experiment-active operations are refused.

`BackendFacade::profileCommand` owns the portable `app/ProfileStore` path. Apply validates
all supported settings before changing stopped runtime services: processing, buffers,
realtime batches/mode, frame delivery, calibration, autofocus configuration, and ROI
bounded to an available preview frame. No camera script is executed, device connected,
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

`desktop/scripts/package-windows-candidate.ps1` creates a fresh portable directory and ZIP: recursive non-system native DLL dependencies (unresolved/conflicting names fail), app-local VC runtime, defaults, isoelastic LUT resources and the pinned YOLO model. The staged application is smoke-launched with development DLL search paths removed. WebView2 Evergreen remains an explicit prerequisite. The candidate disables EGrabber, MindVision and CoreMOR SDKs; SDK-enabled camera delivery and Windows hardware acceptance remain separate gates. Windows hosted execution is required before declaring this candidate validated.

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
