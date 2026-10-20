## 2026-10-20 — Inspect commands in the review contract: E-LUT, track rows, reprocess one record (#717 B3b)

Track B (#717) step B3, the backend half of R3 Inspect (#269). Review ABI
2 → 3 (`review-contract.json`, reserved for B3b on #717; S12 takes 4 or
higher); the bridge ABI is untouched.

- `fetch_review_info.identity` (and the probe's) gains `emodulus_lut`, the
  run snapshot v3 E-modulus LUT provenance (LUT id, revision, source,
  sha256, catalog sha256, checksum status), and `profile_id`. Absent is
  `has_*` false over the bridge and `null` in the shell's JSON.
- `fetch_review_track_rows(valid, track_id, max)`: the records of one track
  in set order with their set positions, `max` at most 256
  (`review_track_rows.max_rows`), `total` and `truncated`. From the metadata
  read at open (`ReviewSession::trackRows`), no HDF5 read; refused for an
  untracked record.
- `review_reprocess_record(valid, index, settings)`: one record's stored row
  next to what the review session's kernel finds in its stored image now,
  with recorded settings (config, ROI, background) or default settings
  (default config, whole image, no background). `settings_label`,
  `core_version` and `core_source` say what was used; `match_index` pairs
  the stored object with the nearest found one inside its bounding box (as
  stored; realtime ROI captures store ROI-local coordinates, open on review
  round 1); `object_count_differs` flags another number of objects. Review
  round 1: it runs at the file's recorded px→µm (`pixel_to_micron`; the
  default for default settings) on a processing instance of its own, so
  area validity matches the recording and no job's instance changes; no
  E-LUT is loaded, so E is not recomputed (`emodulus_recomputed` false). The kernel's frame-level "no object" result is not returned as an
  object. It never writes. HDF5 is not thread-safe (#722): the inputs are
  read under the job lanes' mutex and the session's
  (`ReviewSession::reprocessInputs`), `processBatch` runs outside every
  lock, and it is refused while either job lane is claimed.
- `reviewBridge.ts` gains the types, `trackRows` and `reprocessRecord`; no
  UI change. R3 Inspect (B3a, on `feat/egrabber-workspace`) is the first
  user. Regenerate provenance and Compare are deferred to #754.

Review round 2 (#756, coordinator ruling on the coordinate space):

- `/experiment_info @object_coordinates` is written from now on:
  `roi_local` by the capture path for an inline-realtime host run with an
  ROI, `image` for async-batch and whole-image runs and by
  `saveMasksToHdf5` (regenerate, reanalysis, the Qt batch dialog). PL-science
  runs write none. The Qt `ExperimentController` is compiled but no longer
  used, so it is unchanged.
- At open the session decides each file's space (`ObjectCoordinates.h`):
  attribute; v3 host snapshot (inline with an ROI: `roi_local`; async batch
  or no ROI: `image`); ROI origin (0, 0) or no ROI area: `image`; a mask
  check over up to 64 records (8 decisive, 90 % agreement); else `unknown`.
- `fetch_review_info` carries `object_coordinates`,
  `object_coordinates_source` and `object_origin_x` / `_y`; the contract
  gains `review_object_coordinates` (`REVIEW_OBJECT_COORDINATES`).
- The reprocess match places the stored record through `toImageSpace`;
  an unknown space is refused with the reason.

Tests: `review.object_coordinates` (new), `review.inspect` (new), `review.run_identity` (LUT and profile),
`tests/review_bridge.rs` in both feature configurations, the Tauri identity
JSON test and the `reviewIpc` golden. See [[ReviewSession]], [[YofoReview]]
and [[Rust-Bridge]].
