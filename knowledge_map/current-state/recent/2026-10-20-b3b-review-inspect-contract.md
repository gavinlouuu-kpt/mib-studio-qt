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
  the stored object with the nearest found one inside its bounding box (both
  in image coordinates); `object_count_differs` flags another number of
  objects. The kernel's frame-level "no object" result is not returned as an
  object. It never writes. HDF5 is not thread-safe (#722): the inputs are
  read under the job lanes' mutex and the session's
  (`ReviewSession::reprocessInputs`), `processBatch` runs outside every
  lock, and it is refused while either job lane is claimed.
- `reviewBridge.ts` gains the types, `trackRows` and `reprocessRecord`; no
  UI change. R3 Inspect (B3a, on `feat/egrabber-workspace`) is the first
  user. Regenerate provenance and Compare are deferred to #754.

Tests: `review.inspect` (new), `review.run_identity` (LUT and profile),
`tests/review_bridge.rs` in both feature configurations, the Tauri identity
JSON test and the `reviewIpc` golden. See [[ReviewSession]], [[YofoReview]]
and [[Rust-Bridge]].
