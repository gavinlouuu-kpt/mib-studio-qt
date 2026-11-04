## 2026-10-20 — R1 Open groups a folder's runs by experiment (#717 B1b part 2)

Track B (#717) step B1, the second half of R1 Open (#269), on
`feat/egrabber-workspace` now that it carries review ABI 2 (#746, sync #752).

- **Probing.** After `review_list_dir`, Open probes the folder's `.h5` /
  `.hdf5` files with `review_probe_runs`. It sends calls of at most 256 files
  (`REVIEW_PROBE.max_paths`) one after another, and shows "Reading N of M
  files" meanwhile.
- **Grouping.** One kit `DataTable` per declared `experiment_id`, newest
  first, runs by `run_index`. The groups that follow:
  - runs whose snapshot names no experiment;
  - legacy files (no run snapshot), whose identity cells read "Not recorded";
  - files that cannot be read, with the per-file message;
  - files not read yet, or not read because the probe was refused. A warn
    `Notice` carries the probe's words, and the rows stay openable.
- **What a group shows.** An experiment's caption gives runs and samples,
  dates, conditions, core, calibration and one status chip. Its runs show
  sample, condition, start, records, core, calibration and the accounting
  chip. Experiment names and methods are not in the files and are not shown.
- **Ordering guard.** The #747 folder-generation guard now also drops probe
  answers of a superseded listing, so a late probe never fills a newer
  folder or overwrites a newer refresh. In review round 1 an unmount also
  supersedes the listing, so no further probe call is sent after it.
- **Open run card.** It shows the open file's identity from
  `fetch_review_info.identity`: snapshot, experiment, run, sample, condition,
  annotation, calibration record and operator. A legacy file says "Not
  recorded · legacy file" once.

Kit: grouped table rows are still missing (#715), so each group is its own
`DataTable` with a caption. Tests: `ReviewApp.test.tsx` (grouping, legacy,
unreadable, the 256 cap, out-of-order probes, a refused probe, opening) and
`open/libraryModel.test.ts`; five mutations each fail a test. See
[[YofoReview]].
