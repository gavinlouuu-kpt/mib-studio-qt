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

## 2026-10-23 graph/gallery continuation

Expand **Population graph recipe**, enable **Use graph**, and analyze the supplied
area 100..110 recipe. Edit/add interval and Boolean nodes with explicit parent
IDs and choose `output`; all evaluation is native. Save/Restore retains the JSON
recipe in this browser only. It does not bind/relink a source or save results.

After analysis, **Load gated gallery** copies the first 12 selected thumbnails
with exact set/ordinal labels. Next/previous pages explicitly reevaluate the
source and gate. Missing images show `Image unavailable`, never disappear from
population counts. Copying under the source lock prevents cross-generation images.

This supersedes the earlier "DAG editing not connected" and "no gated gallery"
limitations only for this bounded JSON-graph and stored-thumbnail slice. Full
visual graph editing, source fingerprints, persistent backend strategies,
Cytoflow app scheduling and live E adaptation remain open.

Validation for this continuation: all seven Review Rust bridge tests pass,
including real HDF5 graph/reduction/gallery identity and unchanged source bytes;
`review.session` (now including gallery reads during source reopen/close) and
`backend.facade_review_session` pass. Shared UI tests and production frontend
build pass. Tailnet HTTPS/WebSocket and browser graph evaluation return 10
selected, mean 104.6, median 104.5 for area 100..110; six valid plus four invalid
thumbnails are displayed. Recipe save, page reload, restore and reevaluation
were verified in the browser. Source-picker automation still stalls at its
native prompt; the synthetic file was loaded through `load_recording`.

The previous QA process was absent on resume; F restarted its owned server on
the same loopback port without changing any tailnet handler. Current PID is
recorded in `data/flowjo-app-qa/server.pid`. No live acquisition started. TSan
remains host-blocked as previously documented; no new TSan pass is claimed.


## Backend strategy and job continuation (2026-10-23)

Same tailnet URL and production app. Review → Population analysis → Source-bound
backend strategies: save a backend revision, load history, evaluate exact revision,
load pinned gallery. Restore creates a new revision. Cytoflow polygon job: submit,
refresh/cancel or reconcile after reconnect. Experiment → Monitoring → Live
population strategies exposes the E adapter without requiring a stored file.

The new slice is verified through actual tailnet WebSocket command dispatch and
shared Review bridge; browser click-through awaits an available browser connector.
Synthetic QA: saved exact plan revision selects 10 rows, mean 104.6; pinned gallery
has the same 10 keys and images; real Cytoflow job returns 10 selected with the same
full-file fingerprint. No hardware acquisition was started. Runtime configuration
uses the existing pinned `/tmp/flowjo-808/venv312` development environment; it is not
an installer/distribution qualification. API and limitations: [production-api](production-api.md).

### Tables/retention/restart validation

The same production server was restarted with its metadata/job journals preserved.
The previously completed real Cytoflow job reopened as terminal with 10 selected.
A retained snapshot evaluated with the recording closed; exact manifest import
appended revision 2 without overwriting revision 1. The revision-1 report was
identical through the live reader and retained projection (same content hash).
Native table: p05 100.45, p25 102.25, p75 106.75, p95 109.1; sample SD
3.204163957519444; CV 3.0632542614908647%; mean 104.6, median 104.5.
Normal and ASan/UBSan native population/graph/statistics tests pass. Full F remains
open as recorded in the ledger; UI click-through remains connector-blocked.

### Plot/QC production validation

Actual tailnet commands saved an attributed QC strategy (one good, one bad, twelve
unlabelled/unknown) and a native PlotSpec. Evaluation returned 1 selected, 1 excluded,
12 unknown. The histogram contained exactly 1 event while its display offset was
99. A real Cytoflow job preserved 1 selected/12 unknown and the exact native QC
fingerprint. No processing-validity inference or hardware acquisition was used.
Native host tests cover bin boundaries, outside-range normalization, offset
invariance, QC unknown/foreign-key handling and manifest attribution round-trip.
Eight Review bridge tests also pass, including invalid metadata directory handling
and refusal to silently retarget an initialized metadata authority.

### Template/comparison continuation (2026-10-23)

Production tailnet WebSocket validated two distinct synthetic recordings (14 and
22 observations; the second has20 processing-valid plus2 processing-invalid), retained snapshots, exact template application with interval
override, native comparison, duplicate-source refusal and immutable finding replay.
Finding `finding-1792755676005`, revision `1`, hash
`sha256:f861750916701b10ad3b69a396f890a36d0ab5344a9ddb41f1a659ce53010401`.
Means 107.57142857142856 and 1757.821141043886; descriptive difference
1650.2497124724573 pixel2. This tests transport/identity, not biological inference.
Native host tests additionally cover paired matching, missing override migration,
source-change receipt replay and finding database reopen. Production workspace has
application and comparison/finding controls; browser connector remains unavailable.
UI build and 10 regression tests pass; no new click-through claim.

Report continuation: real tailnet app built two blocks (native population table +
pinned finding), replayed exactly, rejected run-name collision and reopened the
immutable artifact. Run `report-1792755932928-run`, hash
`sha256:abd32d22da268f5cfa4b92fbe69590d1fdc9152987cd89229f49401df0b2021d`.
Native tests additionally verify changed-layout independence and DB reopen. Twelve
UI regression tests pass, including lost report reply and HTML-title escaping.
Browser click-through/export rendering is not claimed while connector unavailable.

Ordered-transform tailnet round trip: plan `transform-e2e-1792756204891` revision1
uses derived `scaled_area = area / 100`. Native selected10, mean1.046 dimensionless;
10 exact gallery keys; real Cytoflow using the derived x column also selects10 and
pins the same parameter fingerprint. Job
`92f82b96a179099b6a01cc93a117ade2b0c95939d8230ea2cd0b908d234c5632`.
Native tests cover ordered linear/log/asinh provenance, invalid log domains staying
unknown, raw-column preservation and plot/job agreement. Worker app tests pass.

Seeded embedding production round trip: plan `embedding-e2e-1792756564772`, job
`99e6deeb569b06b9b8cb2f061143fde616037b00fa85bd85e6f8bb6573b97a5d`.
10 selected eligible events, explicit maxPoints8 => 8 exact-key finite coordinates,
seed42/perplexity2/500 iterations, sklearn1.9.1. Same-runtime reordered-input seeded
repeatability, tampered key/config refusal and fresh-store reopen pass in Rust.
Seven Rust worker, two scheduler, eight Python adapter and twelve UI tests pass.
The new plot is built but browser rendering remains unverified.

Sample library production proof: `study-1792757005646` revision1, two distinct
retained sources and declared groups. Exact save replay, duplicate source rejection,
sample precision6 over library3 over app defaults and open pass over tailnet.
Hash `sha256:d01ff89af038a1f9f58d4588f625bf8dbf2f7c9c0e352a202722147a947fe024`.
Native library DB reopen and undeclared-group refusal also pass. Relink QA opened a
byte-identical copy under a new path: existing saved plan selects10 and returns10
matching images. A different copy with altered bytes is refused `source_mismatch`.
Original synthetic file is unchanged and reopened after QA.

Saved scatter production proof: `scatter-1792757960201` revision1, 14 selected/eligible
but explicit display budget5. Bounds cover all14 (area100–118, deformability0.1–0.28),
not only display points. Repeat reads reproduce hash
`sha256:4c687b2891d39165f2a8b4b631ad7a129c0a3430db839fb194b4087cee7f97f6`.
Normal native host and all4 ASan/UBSan standalone tests pass. Thirteen UI tests pass,
including empty/extreme finite scatter coordinate rendering and exact large keys.
No browser click-through claim.

Multi-source overlay production proof: `overlay-1792758294811` revision1. Native
counts14 and22 retained separately, fraction normalization sums to1 per layer;
display offset0.5 does not alter values. Artifact hash
`sha256:31b357ca7df8325793eaa39946a0ae5f30d7857e9481f7ceb785b3671e03551f`.
Immutable report run `overlay-report-1792758294811-run` contains the exact overlay
hash; report hash `sha256:453844183b21a3c7c638d0e8809f0e3bc987422af894f93ef26f8ca523e63e02`.
Native tests reject mismatched bins and preserve counts despite offsets. All4
ASan/UBSan standalone tests and13 UI regression tests pass. Browser unavailable.

Multi-feature production embedding: job
`3c21a32ddfe7b8e5227e06368afc43db57d52da85dd880504c5de52878a27cc6` pins
area/deformability/derived scaled_area,10 eligible and8 displayed points. Previous
job `99e6deeb569b06b9b8cb2f061143fde616037b00fa85bd85e6f8bb6573b97a5d`
reopened successfully after this extension. The real worker regression uses3
features with one missing selected event and verifies explicit missing count,
reordered-input repeatability, tamper rejection and journal reopen. Seven worker,
two scheduler,13 UI tests and clippy pass.

### Graph editing / revision continuation

Real tailnet backend plan `graph-revision-1792759318653`: revision1 selected14,
revision2 (same draft label, minimum1000) selected0. Graph fingerprints differ;
revision1 re-evaluation is unchanged, exact command replay returns revision2, and a
new command with stale expectedRevision1 returns conflict. Focused UI14 tests pass,
including same-plan CAS retention and exact method/application/QC restoration;
frontend build and native normal plus ASan/UBSan host tests pass. The form editor
has not received browser click-through qualification; connector remains unavailable.

Rectangle production proof: `rectangle-1792759502927` revision1, area[100,106]
AND deformability[0.1,0.2], selected7 and7 exact-key gallery images. Native output
matches an independent check of the frozen source rows. Form/UI regressions cover
name collisions/reversed bounds and refusal to save an unloaded selected revision.

Reusable table production proof: `custom-table-1792759719340` revision1 preserves
columns population/feature/unit/median/selected and population order rectangle then
rectangle-x in native JSON, CSV and manifest. Both rows select7, median103 pixel2.
Report hash `sha256:2a8b996815d366f84e22b953d354b093045365fa7f1e7f5bf279c4ab58a4c36c`.
Native normal and4 sanitizer tests, UI19 regressions and frontend/server builds pass.

Catalog tailnet proof: limit1 strategy pages returned comparison-1792755676005-0
then comparison-1792755676005-1, both exact revision1. Library discovery reopened
study-1792757005646 revision1 with unchanged hash
sha256:d01ff89af038a1f9f58d4588f625bf8dbf2f7c9c0e352a202722147a947fe024.
Native3 host/store suites,4 sanitizer suites, UI20 and production builds pass.
Original synthetic recording restored after the owned server restart; no acquisition.

Scatter drag authoring: component tests verify reversed drags under2× screen scaling,
correct X/Y raw bounds, pointer cancellation and tiny-gesture refusal. Native
rectangle evaluation/image proof remains as above. Browser inventory was checked
again and returned `[]`; real pointer click-through is explicitly unverified.

Ellipse tailnet proof: plan `ellipse-e2e-1792760361901` revision1, center(105,0.15),
radii(5,0.05), area/deformability domain. Native selected7, mean105 pixel2;7 exact
selected-key images; real Cytoflow job
`79b4da5a33c3210de534f53717365f939599a8f4ffca396e13ef4da0ef1144f0`
selected the same7 with matching source/parameter identity. Exact save replay passed.
