# F app integration QA — native stored-population slice

This is real production UI/transport/backend code in the existing
`feat/808-flowjo-realignment` branch, not a new demo UI. The QA input is synthetic.
The branch already contains `b1110256`, `7a11219c`, `1278cfe5`, `4724633a`, and
`4f384c56`; no cherry-pick, reset or main/develop merge was needed.

## Current app surface

- Studio: `desktop/src/App.tsx` → shared `PopulationAnalysis` → `analyze_population`
  → mib-app-commands → backend bridge → BackendFacade → ReviewJobs → ReviewSession.
- YOFO Review: `ReviewPanel` → same component → `review_analyze_population`
  → review bridge → ReviewJobs → same ReviewSession/native science.
- App schema `review-population/1`; backend ABI 46 and Review ABI 8. Existing
  bridge 45 and Review 7 reservations are not repurposed or claimed implemented.
- The actual `yofo-studio-server` serves the built Studio app and real backend at
  `http://127.0.0.1:18808/`. It binds loopback only, uses its own
  `data/flowjo-app-qa` directory and is left running for coordinator inspection.
  Process owner: F; PID and log are in that directory's `server.pid` / `server.log`.
  No other runtime is stopped/replaced. The camera is mock, no capture was started.

## Reproduce and inspect

From this worktree (system fmt avoids this host's mixed Homebrew/system headers):

```sh
cmake --preset linux-backend-only -DMIB_ENABLE_MINDVISION=OFF -DMIB_USE_SENTRY=OFF -Dfmt_DIR=/usr/lib/x86_64-linux-gnu/cmake/fmt
cmake --build build/linux-backend --target mib_backend -j 4
npm ci --prefix desktop
npm run build --prefix desktop
MIB_BRIDGE_NO_CMAKE=1 cargo build --manifest-path crates/mib-bridge-server/Cargo.toml
MIB_BRIDGE_NO_CMAKE=1 cargo run --manifest-path crates/mib-bridge/Cargo.toml --features review-only,contract-fixtures --example review_fixture -- data/flowjo-app-qa/synthetic.h5
```

Start only if the QA port is free (do not replace an existing process):

```sh
crates/mib-bridge-server/target/debug/yofo-studio-server --listen 127.0.0.1:18808 --no-token --data-dir "$PWD/data/flowjo-app-qa" --dist "$PWD/desktop/dist"
```

Open Studio → Review → Select HDF File → the synthetic HDF5 above. Expand
**Population analysis · stored observations** → leave bounds blank →
**Analyze population**. Expect 14 selected, 0 excluded, 0 unknown (10 valid +
4 invalid); area minimum 100, maximum 118, mean 107.57142857142856, median 106.5.
Set minimum 1000 → analyze: 0 selected / 14 excluded; reductions show dashes
(native null, not zero). Reverse bounds → explicit refusal. Change gate/source:
old results disappear. No scientific QC is silently inferred from processing validity.

The browser automation stalled at the existing native prompt for source loading.
For this inspection only, the fixture was opened using the existing server
`load_recording` command, then the page's normal runtime restoration was reloaded.
The analysis disclosure, both Analyze actions and empty/nonempty results were
verified by real browser clicks; the source-picker prompt itself was not verified.
A successful result is left open in the QA browser tab. Read-only analysis also
works for a viewer; source changes remain controller-only.

## Validation

- Backend and Review native libraries build; Review-only Tauri app builds.
- All 7 Review bridge tests pass, including real HDF5 both-set reductions,
  exact set-qualified ordinals, empty results, invalid bounds, stale path and
  byte-for-byte unchanged source.
- Studio/Review native parity test passes through both real bridges.
- Frontend production build and 25 targeted tests pass (transport mocked in UI
  unit tests, not in browser QA).
- Three native population/graph/statistics tests pass under ASan/UBSan.
- ReviewSession fault tests reject unreadable/count-incomplete populations;
  a missing-area-column fixture proves unknown membership rather than default zero;
  normal concurrent open/close/source-replacement stress is included.
- TSan cannot start on this host: even a minimal thread probe exits 66 with
  `FATAL: ThreadSanitizer: unexpected memory mapping`. No TSan pass claimed.

## Concrete remaining integration work

1. Native core supports more than this first interval UI: DAG editing, polygon
   submission to the existing real Cytoflow worker, cancellable app jobs and
   journal recovery are not yet connected to this app surface.
2. Source fingerprints are null and evaluation IDs are process-local, not saved
   artifact identities. No verified reopen/relink or durable workspace strategy.
3. Absent columns and nonfinite values are unknown; detailed per-event missingness
   reasons, calibrated feature/provenance selection and scientific QC still
   require the typed source adapter. E values are deliberately not exposed here.
4. E live provider needs epoch-qualified keys, typed batch coverage/watermarks,
   unknown-denominator handling and valid/invalid/missing measurement fixtures.
   No E/I commit is silently imported, no live acquisition coexistence is claimed,
   and #722 stays enforced by the existing idle/job/session fences.
5. Images are the existing saved-image viewer, not an exact gated gallery. No
   saved applications/overrides, comparison/report/export or full F01–F16 acceptance.

These are production implementation/validation gaps, **not shared-file ownership
blockers**. Original scope remains [capability ledger](capability-ledger.md),
[contracts](contracts.md), [source proposal](event-snapshot-contract.md) and
[execution plan](../../exec-plans/active/2026-10-22-flowjo-realignment.md).
