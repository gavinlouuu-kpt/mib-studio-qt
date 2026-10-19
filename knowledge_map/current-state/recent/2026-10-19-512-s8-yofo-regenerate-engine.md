## 2026-10-19 — YOFO Review's Regenerate masks runs on the reanalysis engine (#512 S8, #735)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S8, decision D6. `ReviewJobs::startRegenerateMasks` builds a
`ReanalysisRequest` and runs `runReanalysis`, the engine behind MIB Studio's
reanalysis. Its own load, median, remap and write code is gone.

The engine request gained three options. Parsing never sets them, so the
facade's output is unchanged:

- `timestamps`: Preserve or Rebase.
- `syntheticBackgroundKind`: QuietTile or MedianOf32.
- `frameOrder` for HDF `all`: Timestamp or Datasets.

YOFO runs Rebase, MedianOf32 and Datasets, its accepted differences from the
2026-10-04 parity sign-off. D6 named two options. The third keeps YOFO's
whole file as valid rows then invalid rows.

YOFO behaviour changes:

- **No clobber.** An existing output is refused at start with "Output
  already exists; choose a new file". An output created while the job runs
  fails the job and is left untouched, and the partial file is removed. The
  Regenerate masks dialog proposes a free name (`<base>_remasked.h5`, then
  `_2`, `_3`, …). It stays open on a refusal with the backend's reason and
  lets the operator choose again.
- **Fix: records are paired with their own frame.** The old job paired
  records with frames by position. Tracking emits one record per track, so on
  the real-cell 512x96 run 1040 of 1049 records named another frame's index
  and timestamp. The engine pairs by the record's frame, as Qt and the facade
  do.
- A folder or AVI with an unreadable image now fails the job, as the facade
  does, instead of skipping the image.

Before and after the change, h5diff showed identical images, masks and
background. On one-record-per-frame fixtures the content was identical too.
The facade path is unchanged. No contract change. See [[YofoReview]] and
[[ReviewSession]].
