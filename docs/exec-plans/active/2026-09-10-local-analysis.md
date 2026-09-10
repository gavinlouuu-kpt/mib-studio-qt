# Desktop local analysis — issue #399

Status: active (native Review foundation and development helper endpoint implemented;
production runtime publication and desktop supervision remain open)

## Goal and release boundary

One desktop installation opens finalized local/authenticated NAS recordings,
performs bounded local Review/Analysis/reanalysis, and creates derivatives only
on explicit request. No separately installed worker, computation server, raw-data
upload, or instrument authority is required for analysis.

This change is the M0 inventory and first M1 implementation, **not completion of
#399**. The full acceptance scenario must remain open until M2–M6 are qualified.
M7 is optional connected functionality; offline operation is the default.

## Inventory and dependency pins

| Area | Existing owner / decision |
|---|---|
| Application lifecycle and permissions | `AppBackend` / `BackendFacade`; immutable `ApplicationMode`, native command allowlist |
| HDF5 review | `Hdf5Service`; read-only file, direct metadata hyperslabs, bounded sparse images |
| React presentation | Existing `desktop/` Review workspace, private Tauri/cxx calls; no localhost worker |
| Science | Existing `mib_processing`, loader/cache/trust registry; no algorithm port into Rust/React |
| Exports | Existing `HdfExportService` (#344); current facade CSV path predates that service and still needs migration |
| Helper-required work | Toolkit histogram/KDE and toolkit review contracts; do not introduce Python into acquisition |
| Toolkit source inspected | `gavinlouuu-kpt/Biowork-toolkit` commit `388924e5c9d95e0691b969be6238f3e94db817d4`, package `0.1.0` |
| Toolkit deployment artifact | **Unresolved**: documented `https://updates.yofo.bio/stable/biowork-toolkit/versions/0.1.0.json` returned HTTP 404 on 2026-09-10; no wheel SHA-256 asserted |
| Native starting revision | `e51c72a2684782ab9cf7328ad71b85d7ab8bc8d8` on develop; #394 compatibility work must continue to resolve through the same registry |
| Conformance | `tests/processing/`, `docs/gold_standard_metrics.md`, `crates/mib-bridge/tests/contract.rs`; reuse these when adding packaged reanalysis |

The toolkit README requires an exact released wheel plus SHA-256 for deployment.
A development checkout is not evidence of a published, compatible distribution.
The source/version above is an inventory pin, not a production bundle lock.
No model/runtime digest or toolkit wheel digest is fabricated.

## Implemented native boundary

- `AppBackend::initialize(path, AnalysisOnly)` suppresses camera selection,
  acquisition/playback bootstrap, processing worker bootstrap, model loading,
  trigger wiring and autofocus construction. Normal instrument initialization
  remains the default. A context cannot escalate to Instrument after creation.
- The facade permits recording open and operation cancellation in analysis mode;
  all other command variants fail closed. Direct hardware discovery/autofocus
  queries and direct background mutation are blocked too.
- Analysis opens require stored terminal run accounting. Legacy/unknown
  finalization is explicitly rejected; instrument Review retains legacy access.
  Failed or intentionally partial finalized runs are inspectable and their
  recorded completion/accounting state is displayed, never called complete.
- Metadata open uses dataset shape only. Measurement reads use a maximum of
  4,096 rows, with no retained full-file metadata cache. Zero rows queries count.
- Sparse image review checks a 32 MiB decoded image budget before reading.
  Metadata inspection queries background geometry without loading pixels.
- Observed source identity is absolute path + size + modification time, checked
  before/after reads. Missing mounts or changed sources reject reads and clear
  outgoing data. This is **observed**, not content-verified identity; same-size,
  same-timestamp replacements are not detected. Full execution/publication needs
  the toolkit's verified snapshot identity before M4/M5 can ship.
- Bridge ABI 14 adds analysis initialization and persisted Review completion
  fields. Raw Review addresses the original HDF5 dataset by row, not the live
  acquisition frame store. Raw recordings do not fabricate scientific metrics.
- Desktop launch supports `--analysis-only` or the immutable Cargo feature
  `analysis-only`. Analysis starts in Review, disables instrument navigation and
  the service-mode control, and disables unqualified derivative creation.

Run the desktop with `MIB_BRIDGE_NO_CMAKE=1 cargo run --manifest-path desktop/src-tauri/Cargo.toml
--features analysis-only` after building the frontend/backend prerequisites in
`knowledge_map/architecture/Desktop-Shell.md`. This is a development launch,
not a qualified standalone installer. Build the native backend with both
`MIB_ENABLE_HARDWARE_SDKS=OFF` and `MIB_ENABLE_MINDVISION=OFF`.

## Frozen helper protocol proposal (M2; not implemented)

The production proposal below remains unimplemented. An experimental protocol
0.1 endpoint now lives in `desktop/analysis/helper.py`; its private-pipe tests
exercise bounded Toolkit histogram/KDE pages, handshake, identity echo, generation
advance/rejection, malformed input, budget limits and process exit. See
`desktop/analysis/README.md` for the exact envelope and development command.
It reports `production_ready: false` and is not connected to the desktop. It
does not implement the production digest handshake, operation supervisor,
independent cancellation, parent-death ownership or whole-dataset analysis.

Use one app-owned child process, launched from a verified bundle-relative path.
No executable supplied by a webview and no PATH/system-Python fallback.
A length-prefixed UTF-8 JSON control channel over inherited private pipes carries
protocol major/minor, request ID, operation ID, dataset generation and method.
No pickle, HTTP listener, network pairing, camera or trigger methods.

Handshake returns exact toolkit version/wheel digest, helper build digest,
protocol versions and explicit supported capabilities. Compare with the signed
application bundle lock before accepting work. Reject missing/mismatched bundles;
report `local_analysis`/`local_reanalysis` unavailable with a reason.

Initial budgets: 1 MiB control messages, one running analysis operation, one
replaceable pending view request, 4,096 measurement rows per transfer, 32 MiB
pixel/result blocks, and 1 MiB retained log ring. Large arrays use app-created
file-backed blocks with byte count, digest and generation; only bounded control
metadata travels in JSON. These are design limits, not measured helper evidence.

Cancellation/control must be serviced independently of computation. The existing
native operation ledger remains authoritative: accepted/running/completed/failed/
cancelled/unknown are explicit; stale generations cannot publish results. Parent
shutdown closes pipes, cancels, then terminates/reaps with a deadline. Add Windows
Job Object ownership and Linux parent-death behavior for forced parent exit.
No automatic retry for writes/publication. Durable local operation evidence must
classify interrupted/unknown jobs on restart before enabling resume.

## Remaining milestones and gates

- [x] Inventory native Review, facade, desktop, exporter, toolkit source and core ownership.
- [x] Add native analysis context and deny instrument commands.
- [x] Replace full metadata cache/open reads with bounded hyperslabs.
- [x] Add observed-source invalidation, persisted completion and sparse raw Review.
- [ ] Finish M0 production bundle lock: published toolkit wheel/digest, Python
  distribution/dependency lock, compatible native artifacts and schema pins.
- [ ] M1 native NAS read deadlines/cancellation (current native reads are synchronous).
- [ ] M1 real Tauri analysis walkthrough on Windows/Linux, including mapped drive,
  UNC and mounted NAS. No hardware SDK requirement in installer qualification.
- [ ] M2 implement the owned helper and packaging; crash/cancel/stale-generation/
  parent-exit/restart tests. A clean machine must not need system Python.
- [ ] M3 shared histogram/KDE/scatter/review contracts and bounded UI retention.
- [ ] M4 pinned authoritative reanalysis, separate result revisions, verified
  source/config/core provenance and native/packaged conformance.
- [ ] M5 route derivative creation through `HdfExportService`, replace its remaining
  full metadata vectors with paging, add source-linked manifests and repeat/cancel
  fault injection. Do not advertise this capability in analysis mode prematurely.
- [ ] M6 one signed installer per supported platform; offline clean-machine and
  NAS disconnect/reconnect qualification; uninstall preserves user data.
- [ ] M7 optional identity/metadata/result synchronization with conflict handling.

## Verification

The native tests cover exact sparse pixels and metadata identities, terminal vs
unverified finalization, command denial, processed/full-reader projection parity,
zero-row counts, oversized requests, uint64 offset overflow, malformed metadata,
100-million-row sparse extent access, HDF handle stability, source immutability,
and missing/changed source behavior. A watchdog bounds native lifecycle tests.

`review_metadata_budget_test` was linked to the unmodified baseline libraries and
failed because an unbounded page request was accepted; it passes with the fix.
The Rust bridge adds an analysis initialization/permission test with a watchdog.
Record executed gates and environment limits in the task note, not as checked
clean-machine milestones above.

## Decision log

- 2026-09-10 continuation: registry investigation found no toolkit tags, releases
  or release workflow runs. The publisher also omitted exact trailing-slash pip
  keys on R2; Toolkit PR #2 fixes this with fail-before/pass-after coverage.
  Published wheels block production packaging, not development at the pinned
  source revision. Continue M2 development without claiming release readiness.

- 2026-09-10: keep native Review native; a Python helper is only for toolkit-owned
  features. Preserve one authoritative scientific implementation.
- 2026-09-10: reject unverified finalization in standalone analysis rather than
  silently treating a legacy or live-growing file as a finalized source.
- 2026-09-10: withhold standalone export until its source identity, bounded
  metadata and existing exporter lifecycle have been integrated and qualified.
