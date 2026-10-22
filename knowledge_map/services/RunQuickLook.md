# RunQuickLook

> The run quick look (#716 A7a, bridge ABI 44 `run_quick_look`, review ABI 6
> `review_run_statistics`): the per-run descriptive statistics of a closed HDF5 experiment run
> for the operation app's Run summary (O5) and Quick look (O6), and for Review (B2 R2). **One C++
> function** computes them for every surface, so their parity holds by construction. Computed at
> close into an in-memory cache, or on demand while the instrument is idle; nothing is ever
> written.

**Source:** `include/backend/review/RunStatistics.h`, `src/backend/review/RunStatistics.cpp`
(`mib_review_core`: median, the accumulator, `computeRunStatistics`, the readers, the LRU, the
request parser and the reply JSON), `include/backend/review/RunStatisticsFixture.h` /
`src/backend/review/RunStatisticsFixture.cpp` (the parity fixture), the column reader
`Hdf5Service::readMetadataColumns`, `ExperimentCoordinator::quickLookAtClose` (step 6b),
`BackendFacade::runQuickLookJson`, `ReviewJobs::runStatistics` / `ReviewSession::runStatistics`.
**Related:** [[../architecture/ExperimentCoordinator]] (the pass at close),
[[../architecture/AppBackend]] (owns the cache), [[ReviewSession]] (the lane and session gate,
the row path), [[Hdf5Service]] (the column reader), [[CalibrationRecords]] (shares `median()`),
[[../architecture/Rust-Bridge]] (ABI 44, review ABI 6), [[../architecture/Desktop-Shell]] (TS
client), [[../frontend/YofoReview]] (the twin), [[../architecture/Threading-Model]].

**Consumers.** The workspace's Experiment sub-views Run summary (O5) and Quick look (O6) (#716 A7b,
A7c, on the workspace branch: `desktop/src/workspace/RunSummaryView.tsx` reads its one run,
`QuickLookView.tsx` the plan's closed runs with the reference; each on its transitions and
Refresh, shows the numbers and deltas as sent, never computes) and Review's B2 R2
(through `review_run_statistics`).

## Responsibility

- **The numbers** (the issue's A7 ruling 2), over the valid set (`/valid_frames/metadata` rows
  with `isValid`, as Review's charts count them):
  - counts: the set's rows, the valid rows, the finite rows (area and deformability both
    finite: the points Review plots) and the excluded rows (valid − finite); the run accounting
    as the file records it;
  - area µm²: n and median, each row as `area_px * (f * f)` (Review's scatter), at the file's
    **recorded** factor only (`ReviewSession::recordedPixelToMicron`: the snapshot's
    `pixel_to_micron`, else a regenerated file's own factor; a `fallback` source is not recorded,
    #778). With none: n, median and factor are null, never a fallback;
  - deformability: n and median over finite values;
  - E kPa: `computed | no_lut | not_recorded`, following the identity's E-LUT record
    (`RunIdentity::eModulusLut` / `eModulusLutProvenance`, review ABI 5, #783): the run snapshot
    v3 `emodulus_lut` (`snapshot`), else a regenerated file's `@emodulus_lut_json` (`recorded` |
    `host`, #764); never the host's current LUT. n and median over finite values > 0;
    `out_of_domain` = valid rows whose E is not finite (`EModulusLut::lookup` returns NaN outside
    its table); zeros (no value) count in neither; the LUT identity (id, revision, source,
    sha256, provenance as the identity names it). A valid set whose compound lacks `youngsModulus` is
    `not_recorded`; an absent valid set lets the record decide;
  - the target group: `declared` from the recorded config document
    (`image_processing.target_group.enabled`; null when the file records no config document),
    n of N valid rows and %, labelled "% in the run's declared target group" (no polygon, Q2 a);
  - the event rate: valid rows ÷ (last − first valid timestamp) on the file's declared clock
    (acquisition provenance `ticksPerSecond`); `not_recorded` without one, `too_short` below two
    rows or without a span;
  - the identity (`ReviewSession::readRunIdentity`) plus the method revision, config hash, core,
    contract, start generation and times;
  - an overlay of at most 2000 finite rows at a deterministic stride (`ceil(finite / 2000)`),
    with area px, area µm² (when recorded), deformability and the target-group bit: display only,
    outside the parity claim.
- **Honesty** (ruling 6). Counters are decimal strings; null means not recorded; a recorded 0
  stays 0. Flags are descriptive, in a fixed order: `low_n` (< 200 valid rows, Q6 a), `loss`
  (accounting intentionally partial, incomplete loss or failed), `legacy` (no snapshot),
  `no_accounting`, `e_out_of_domain`, `factor_not_recorded`, `run_not_finalized` (a snapshot but
  no accounting: the snapshot is written at Start, the accounting at finalization). No verdicts,
  tests, fits, CV/IQR or recommendations (ruling 10).
- **Deltas** (ruling 5). `run − reference` per median, computed here (the UI does no
  arithmetic). Withheld with a reason: area `factor_not_recorded` (either side); E `no_emodulus`
  (either side not computed or without a median), `lut_unidentified` (a hash missing),
  `lut_differs`; `no_value` for a missing median. A different profile, method revision (central
  revision when both have one, else the config hash), core or contract is flagged in `differs`
  (`same | differs | unknown`), never withheld. No % change, no significance. The reference is
  client-held (path, run_id, start_generation, Q3 a); the backend confirms its `run_id` against
  the file (null = the file records none) and echoes the start generation. Not persisted.
- **When computed** (ruling 3). At close: finalization step 6b after the calibration record and
  before the terminal status ([[../architecture/ExperimentCoordinator]]); it never fails the run
  and its time is logged. On demand (`run_quick_look`): a hit in the LRU (64 runs, keyed by
  canonical path, size and modification time; each entry carries its run_id) needs no HDF5 read
  and is served during a run. A miss is read only inside the coordinator's idle transaction, with
  no raw recording, the review lanes free and under the review session's mutex, one file per
  transaction; otherwise the file is `deferred` ("computed when the instrument is idle"), never an
  error. A raw recording is `not_available` ("not available until exported to HDF5"); SSD-raw
  runs have no HDF5 file until they are exported.
- **Nothing written.** The readers open files read-only (`Hdf5Service::loadFile`); the cache is
  in memory only.

## Key APIs

- `median(std::vector<double>)` (also `app::deriveCalibration`'s).
- `RunStatisticsRows::add` (the accumulator both read paths feed), `computeRunStatistics(rows,
  header)`, `readRunStatisticsHeader(reader)`, `readRunStatisticsRows(reader, rows, error,
  chunkRows)` (the column path), `runStatisticsRowsFromFrames(frames)` (the row path),
  `readRunStatistics(path, computedAt)` (one file, its own reader).
- `stampRunFile`, `RunStatisticsCache::find / insert` (thread-safe; a changed stamp drops the
  stale entry).
- `parseRunQuickLookRequest`, `runQuickLookReplyJson`, `runQuickLookRefusedJson`,
  `runStatisticsJson`.
- `ReviewSession::runStatistics(path)` (session mutex), `ReviewJobs::runStatistics(path)` (lanes'
  lock; `Deferred` while a lane is claimed).
- `ExperimentCoordinator::setClosedRunStatisticsReader` (the facade installs
  `ReviewJobs::runStatistics`), `AppBackend::runStatisticsCache()`,
  `BackendFacade::runQuickLookJson`.
- Fixture: `writeRunStatisticsFixture(path, variant)` (V3WithE, Legacy, LutNotLoaded,
  V3WithEEven, EmptyValidSet, RawRecording); `review_fixture_write_quick_look` on the review bridge.

## Memory (the PZ7035)

The column path reads six members (`timestampNs`, `area`, `deformability`, `youngsModulus`,
`isValid`, `isTargetGroup`) in chunks of 65536 rows (`Hdf5Service::kMetadataColumnChunkRows`,
about 2.5 MiB a chunk), never the whole compound row and never every row at once. The
accumulator keeps about 17 bytes per valid row (area, deformability, target-group bit) plus 8 per
E value; the medians compact the columns in place. The row path (`ReviewSession`'s
`ProcessedFrame`s) is used only by the parity tests.

Measured on the desktop (`performance.run_quick_look_memory`, 2026-10-22, Release; the heap trimmed
before each measurement):

| Rows (valid) | Column path (the close pass, `run_quick_look`) | Row path (ReviewSession open, for comparison) |
|---|---|---|
| 100k (88,888) | 11 ms, peak +10.5 MB | 116 ms, peak +123 MB |
| 1M (888,888) | 93 ms, peak +41 MB | 1.14 s, peak +1.23 GB |

The PZ7035 is unmeasured (A7a did not touch the board); the row path's 1.2 GB at 1M rows is why
the pass never takes it there.

## Gotchas

- The run's own file at close: the pass reads it after `closeFile()`; a review job holding a lane
  at that moment defers it (nothing cached), and the first on-demand call when the instrument is
  idle computes it.
- Without the facade's reader (a native shell) the pass reads nothing: no lock would serialise it
  against that shell's own HDF5 users (#722).
- Never a fabricated 0: a valid set whose compound lacks `area`, `deformability` or `isValid` is
  an error; without `isTargetGroup` the group's n, of and % are null (declared stays as recorded);
  without `timestampNs` the event rate is `not_recorded`. The target group counts as declared only
  with a boolean `image_processing.target_group.enabled` and every recorded member of its exact
  type (area bounds integers in int range, never `true` as 1 or 10.5 truncated; the other bounds
  finite numbers; the switches booleans);
  a bound the document does not record is null. The contract version is reported only when a
  marker is recorded (the reader's legacy Contract 1 is an inference, not a record).
- Opening a file the review session already holds with a second reader is fine (HDF5 shares the
  file); the session keeps reading its own handle (tested).
- The file's modification time keys the cache only (std::filesystem's clock has no portable epoch
  in C++17), so the reply reports canonical path and size, not the time.
- The review twin has no cache and no instrument: only a running review job defers a file, and
  paths are used as given (the backend bridge resolves them as client paths, #762).
- Review ABI 6 builds on 5 (#783, the identity's E-LUT provenance, which the E status uses).

## Tests

`review.run_statistics` (median; V3WithE by hand: counts, area, deformability, E with
out_of_domain, target group, event rate, accounting, flags, identity, overlay; legacy, no LUT,
even n, empty set, raw, missing / corrupt; regenerated files whose `@emodulus_lut_json` is `host`, `recorded` (not loaded) or an unknown provenance, and the snapshot record winning over the attribute; **the column path byte-equal to ReviewSession's row
path** for every fixture at chunk sizes 1, 7, 25 and the default, and on 70000 rows across the
chunk boundary and the overlay cap; deltas and the reference; the strict parser; no writes; the
LRU (bound, eviction, refresh, stale stamps) under six threads for TSan; the session left as it
was; `ReviewJobs::runStatistics` deferred while a job holds a lane), `e2e.run_quick_look`
(through the facade: the close pass before the terminal status, deferred during a run and during
a review job, cache hits during a run, the close pass deferred by a held lane, the reference, no
actuation, refusals). Rust: `contract.rs` `run_quick_look_is_described`,
`run_quick_look_equals_review_run_statistics` (both bridges, every shared field, computed and
cached); `review_bridge.rs` `run_statistics_twin_over_the_parity_fixture` (pins the golden
`contract/fixtures/run-quick-look-v1.json`); `dispatch.rs` `run_quick_look_through_dispatch`;
`ws.rs` `run_quick_look_over_the_websocket` (a viewer reads); the server's `run_quick_look_is_a_read`.
TypeScript: `runQuickLook.test.ts`.
