## 2026-10-19 — Run identity in the review contract (#717 B1a)

Track B (#717) step B1, the backend half of R1 Open (#269). Review ABI 1 → 2
(`review-contract.json`, reserved on #717); the bridge ABI is untouched.

- `fetch_review_info` gains `identity`: the open file's run snapshot version,
  run id, experiment id, run index, sample ref, calibration record id,
  operator and planned annotation (fixed members + free key/values), read from
  run snapshot v3 (#712) and `/experiment_annotation` by
  `ReviewSession::readRunIdentity`. A legacy file reads every member absent
  (`has_*` false over the bridge, `null` in the shell's JSON), never 0 or "";
  an attribute that is not a JSON object reads as unreadable.
- `review_probe_runs(paths)` is the experiment library's header-only probe:
  identity, start/end time, counts, run accounting and core version per file,
  no images; at most 256 paths (`review_probe.max_paths`); a missing,
  non-HDF5 or locked file is a per-file error.
- HDF5 is built without thread safety (#722), so the probe reads each file
  under `ReviewSession`'s own mutex and holds the job lane's start lock for the
  whole call (`ReviewJobs::probeRuns`), refusing while a job runs. The open
  file is left as it was, also when it is probed itself.
- `reviewBridge.ts` gains the types and `probeRuns`; no UI change.

Tests: `review.run_identity`, `tests/review_bridge.rs` (both feature
configurations), the Tauri identity JSON test and the `reviewIpc` golden. See
[[ReviewSession]], [[YofoReview]] and [[Rust-Bridge]].
