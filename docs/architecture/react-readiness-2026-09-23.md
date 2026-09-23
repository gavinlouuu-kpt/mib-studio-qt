# React/Tauri migration readiness — 2026-09-23

## Verdict and scope

**Software parity is now substantially complete; do not remove Qt yet.** The
operator workflows and public contracts called out in the original assessment
have since been implemented on `dev/react-tauri` and exercised through the
native mock workflow. The remaining blockers are delivery evidence (Windows,
installer/release feed and platform packaging) plus the deliberately deferred
SDK-equipped and physical hardware acceptance. This document is retained as a
dated assessment, with the current state recorded below.

The original assessment was against freshly fetched `origin/develop` at
`2fe0282` and correctly found the Tauri branch behind it. The implementation
continued on `dev/react-tauri`; its current state and evidence are tracked in
`docs/exec-plans/active/2026-09-23-tauri-replacement.md`. The original checkout
(`feat/ultra96-direct-ddr`) contains unrelated uncommitted work and remains
preserved. Issue #372 defines the integration gates; the July migration matrix
and the gap list below are historical, not current blockers.

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

## Historical gaps — closed on `dev/react-tauri`

### Existing bridge, missing operator controls

Camera script/MindVision JSON editing, checked Save As, pump/autofocus controls,
typed nanopositioner endpoints and pulse-generator controls are now wired with
guarded operation states. The controls remain disabled only when their current
backend safety gate is not satisfied.

Preview pause/scrub/save, coherent processed overlays, profiles, full-data
monitoring/review charts, batch metrics, shared exports, reanalysis and mask
regeneration are implemented. Native acceptance covers the primary capture →
finalize → reopen → export path and the packaged Linux reanalysis path.

### Historical contract gaps — closed on `dev/react-tauri`

Checked configuration transactions preserve drafts and reject stale revisions;
frame packets carry acquisition/store epochs and processed previews retain
source/recipe identity; recovery reconciles retained native state; exports use
the shared cancellable service; typed hardware and startup discovery paths are
bridged. These claims are software-tested only until SDK-equipped hardware
acceptance is run.

## Current remaining acceptance and release gates

- Windows SDK-free candidate build, smoke and runtime closure are still a
  hosted result to be confirmed for the current revision.
- A platform-specific signed Tauri feed, real installer acceptance and rollback
  evidence remain release gates. No feed or release has been published.
- SDK-equipped deployment, hardware timing and the full hardware suite remain
  explicitly deferred; mock, fake-serial and SDK-free checks do not establish
  those claims.
- Performance budgets and long-run hardware stability still require the later
  joint acceptance run.

## Verification executed

- `npm ci --no-audit --no-fund`: succeeded.
- `npm test`: **258 passed, 37 files** on the integrated branch.
- `npm run build`: TypeScript and Vite production build passed.
- `python3 scripts/gen_bridge_contract.py --check`: passed.
- `python3 scripts/check_docs.py`: passed before this assessment was added.
- Integrated branch native workflow, packaging and backend/bridge checks are
  recorded in `docs/exec-plans/active/2026-09-23-tauri-replacement.md` and the
  draft PR #450. The current `60cd6c8` Desktop CI run is green; the remaining
  hosted checks are still in progress.

CI is evidence for its exact commit and configured checks, not a newly executed
native build of `2fe0282`. No hardware was actuated, native GUI launched, or
full CTest/Cargo suite rerun locally in this assessment.

## Recommended completion order

1. Finish hosted Windows and release-package verification for the integrated
   branch.
2. Establish a signed, platform-specific Tauri feed and test installer update
   and rollback in a disposable environment.
3. Run the deferred SDK-equipped and physical hardware suite, including timing
   measurements and long-run stability, then compare against Qt on the same
   hardware/configuration.

References: [integration issue](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/372),
[migration epic](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/246),
`docs/exec-plans/active/2026-09-07-agent-b-react-tauri.md`,
`docs/exec-plans/active/2026-09-08-agent-a-handoff-372.md`,
`docs/exec-plans/tech-debt-tracker.md`.
