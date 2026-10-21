## 2026-10-22 — Run quick look: per-run statistics at close and on demand (#716 A7a, bridge ABI 44, review ABI 6)

The operation app's Run summary (O5) and Quick look (O6), and Review's B2 R2, need per-run
descriptive numbers that nothing computed before. Per the issue's A7 rulings, one C++ function in
`mib_review_core` (`backend::review::computeRunStatistics`, `RunStatistics.{h,cpp}`) computes them
for every surface, so the parity between the operation app and Review holds by construction:
counts (set, valid, finite, excluded) and the accounting; area µm² at the file's recorded factor
only; deformability; E (`computed | no_lut | not_recorded` from the identity's E-LUT record and
its provenance, #783's `snapshot | recorded | host`, n and
median over finite values > 0, `out_of_domain`, the LUT identity); the declared target group; the
event rate on the file's declared clock; the identity; descriptive flags; a display-only overlay of
at most 2000 points. Null means not recorded; counters are decimal strings. `run − reference`
deltas per median are computed by the backend, withheld with a reason (E when the LUT hashes differ
or either side has no E; area when a factor is not recorded) and flagged, not withheld, for a
different profile, method revision, core or contract.

When: in the coordinator's finalization step 6b, after the calibration record and before the
terminal status (never failing the run; timed in the log), into an in-memory LRU of 64 runs
(`AppBackend::runStatisticsCache`, keyed by canonical path, size and mtime; never persisted). On
demand, `run_quick_look {paths 1..32, reference?}` serves hits during a run and reads a miss only
inside the idle transaction with the review lanes free and under the session's mutex (#722);
otherwise the file is `deferred`. The valid set is read as six columns in bounded chunks
(`Hdf5Service::readMetadataColumns`), never whole rows, for the PZ7035's RAM. Nothing is written
to a file, a store or a snapshot. `CalibrationRecord` now uses the same `median()`.

Bridge: `run_quick_look` (a read for every client on the WebSocket server; `mib-app-commands`,
Tauri, `bridge.ts` / `runQuickLook.ts`) and the review twin `review_run_statistics` (review ABI 6,
building on #783's 5). Parity fixture `RunStatisticsFixture.h` (v3 with E, legacy, LUT not loaded, even n,
empty valid set, raw), the column path byte-equal to ReviewSession's row path, and a Rust test
that both bridges agree. No UI: A7b brings the Summary and Quick look on the workspace branch. See
[[services/RunQuickLook]], [[architecture/ExperimentCoordinator]] and [[architecture/Rust-Bridge]].
