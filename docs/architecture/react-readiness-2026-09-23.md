# React/Tauri migration readiness — 2026-09-23

## Verdict and scope

**Continue migration; do not remove Qt yet.** The reusable backend and transport
are substantially implemented, but operator workflow parity, several public
contracts, native workflow acceptance and release delivery remain incomplete.
This is a source/CI assessment, not hardware or native-webview acceptance.

Assessed freshly fetched `origin/develop` at `2fe0282`, in an isolated worktree.
`origin/dev/react-tauri` (`5c7918c`, July 21) is an ancestor, 252 commits behind;
it is not the correct starting point for renewed implementation. The original
checkout (`feat/ultra96-direct-ddr`) contains unrelated uncommitted work and
was preserved. Unmerged feature branches and that dirty work are not included
in the readiness verdict. Issue #372 defines the newer integration gates;
the July migration matrix is stale and cannot establish current coverage.

## Implemented foundations

- Qt-free backend architecture; shared facade and backend-owned experiment
  readiness, lifecycle, finalization and accounting.
- Versioned C++/Rust/Tauri bridge (ABI 14), generated contract checks, exact
  integer event transport, atomic binary frame packets and bounded frame pulls.
- React paths for mock/hardware camera selection, capture, raw recording,
  processing JSON/ROI/background, experiment start/stop, monitoring rows,
  trigger actions, HDF5 review images/metrics and CSV export.
- Asynchronous discovery APIs, pump/autofocus command surfaces, platform paths,
  preferences and shell logging. API availability is not UI completion.

## Concrete gaps

### Existing bridge, missing operator controls

`desktop/src/App.tsx` still disables camera-script Reset/Save/Apply/Browse
(lines 1353–1364 and 1547–1555), although `bridge.ts` exposes
`applyCameraScript` and `resetHardwareCamera` (lines 456–458).
Pump connect/configuration/run/purge APIs and autofocus connect/jog/config APIs
exist (bridge.ts 374–409), but App.tsx has no corresponding action calls;
status/preflight displays do not replace those controls.

ROI overlay (1302), preview buffer save/scrub (1452–1458), profile management
(1480–1485), monitoring charts (1692–1704), Review charts (1812), and review
Export All/Batch Metrics/Regenerate Masks (1768–1770) remain absent/disabled.
Some missing workflows need backend expansion as well as UI work. Disabled
tooltips still say “not bridged” even where bridge methods now exist.

### Missing or insufficient public contracts

- **Configuration:** React applies merged processing JSON, not a checked
  persistent transaction with baseline revision, external-edit conflict,
  saved/applied/readback outcomes. Profiles and the newer Qt configuration
  workflow cannot be replaced by the current JSON editor.
- **Frame identity:** `desktop/src-tauri/src/frame_packet.rs:34` explicitly
  leaves source/session/config identities zero and timestamp clock/validity
  unknown. Atomic pixels+metadata is fixed; authoritative overlay/source
  identity and time semantics are not complete.
- **Recovery:** experiment terminal status is queryable, but the broader
  session/watermark snapshot and retained operation outcomes described in
  handoff gaps G1/G4/G5 are not delivered as a general facade recovery API.
- **Export:** `BackendFacade.cpp:1393` implements the legacy CSV worker.
  The reusable `HdfExportService` has no corresponding facade job binding
  for the full export workflow/progress/cancel/partial-output contract.
- **New hardware:** autofocus facade Connect still accepts a numeric COM port
  and calls the legacy connection overload (BackendFacade.cpp:1673), rather
  than the backend's newer typed nanopositioner Endpoint/vendor contract.
  Pulse-generator configuration/control is not exposed by bridge.ts. Existing
  backend illuminated-capture orchestration must be reused, not recreated.
- **Startup:** tech debt TD-13 records that Tauri does not start the startup
  discovery coordinator; enabling it needs correct bridge-mutex execution.

### Remaining acceptance and release gates

- M2 recovery, M3 integrated workspace/drafts/layout, and M4 actual native
  configure → readiness → start → stop/finalize → reopen → export remain
  unaccepted in the integration plan. Issue #372 and migration epic #246 are open.
- Monitoring still toggles with view visibility and uses asynchronous interval
  polling (App.tsx:370). Backend accounting is separate: this is not evidence
  that hiding the view loses recordings. Hidden/stalled-view independence and
  bounded poll ownership still need the specified end-to-end proof.
- No same-backend, fixed-fixture Qt/Tauri scientific/config/accounting comparison
  was established by this assessment. Qt bench evidence is not Tauri evidence.
- The GUI smoke script checks process survival only; it does not click through
  a production webview workflow or verify resulting HDF5 files.
- `tauri.conf.json` has `bundle.active: false`; Updates is disabled in App.tsx.
  Updater manifest/hash helpers exist, but do not establish an installed-app
  update/rollback workflow. Native platform acceptance and measured cutover
  performance budgets are still required; the July budget table remains TBD.

## Verification executed

- `npm ci --no-audit --no-fund`: succeeded.
- `npm test`: **124 passed, 11 files**.
- `npm run build`: TypeScript and Vite production build passed.
- `python3 scripts/gen_bridge_contract.py --check`: passed.
- `python3 scripts/check_docs.py`: passed before this assessment was added.
- Latest queried develop Desktop CI: success at `eba55c6`,
  https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/35576575394
- Latest queried develop Bridge CI: success at `eba55c6`,
  https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/35576575381

CI is evidence for its exact commit and configured checks, not a newly executed
native build of `2fe0282`. No hardware was actuated, native GUI launched, or
full CTest/Cargo suite rerun locally in this assessment.

## Recommended completion order

1. Use current develop and replace stale parity statements with a tracked
   inventory: backend service → facade → Rust → Tauri → React → acceptance.
2. Close configuration transactions, identity/recovery and shared-export
   contracts; extend typed endpoint/pulse-generator APIs for current hardware.
3. Wire existing camera/pump/autofocus APIs and finish the required controls,
   retaining one shared backend implementation and explicit operation states.
4. Prove the real native mock workflow and failure/close/cancel cases; compare
   Qt and Tauri on the exact same backend/config/admitted frame fixture.
5. Complete native platform, layout/resource/performance, packaging and updater
   acceptance before authorizing Qt removal. Hardware timing acceptance uses
   oscilloscope measurements, not SDK/UI state alone.

References: [integration issue](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/372),
[migration epic](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/246),
`docs/exec-plans/active/2026-09-07-agent-b-react-tauri.md`,
`docs/exec-plans/active/2026-09-08-agent-a-handoff-372.md`,
`docs/exec-plans/tech-debt-tracker.md`.
