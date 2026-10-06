# Review parity harness (YOFO Review vs the Qt Review tab)

Evidence for the parity sign-off of
[`2026-10-01-standalone-review-app.md`](../../docs/exec-plans/active/2026-10-01-standalone-review-app.md)
(PR 8). For each input file the harness records what each shell shows and
exports, then compares:

| Half | Program | Writes `<out>/<stem>/…` |
|---|---|---|
| Qt tab | `tests/frontend/review_parity_qt_dump.cpp` (ctest `parity.review_qt_dump`, offscreen `HdfReviewTab`) | `qt/summary.json` (status text, px→µm, scatter count + axis ranges, histogram bins/labels/y range), `qt/metrics.csv`, `qt/all/`, `qt/core.json` |
| YOFO Review | `crates/mib-bridge/examples/review_parity.rs` (the review bridge, as the app calls it) | `yofo/summary.json`, `yofo/scatter.json`, `yofo/metrics.csv`, `yofo/all/`, `yofo/core.json` |
| YOFO Review TS | `desktop/scripts/review-parity-charts.test.ts` (vitest, opt-in) | `yofo/charts.json`: `paddedExtent`, `ringRatioHistogram`, histogram y range |
| Compare | `compare.py` | `report.md`: PASS / ACCEPTED / DIFF per check |

```bash
cmake --preset linux-system-release && cmake --build build/linux-system --target mib_frontend_tests
cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build --target mib_review_core
(cd desktop && npm install)
tools/review_parity/run.sh <out-dir> <file.h5> [<file.h5> …]   # exit 1 on an unaccepted DIFF
```

Inputs used for the sign-off:

- the `z-adjustment-50v` corpus (`python3 scripts/provision-assets.py --asset z-adjustment-50v`), a recording file;
- the real-cell 512x96 run that `integration.review_scatter_e2e` records
  (`MIB_REVIEW_E2E_KEEP_H5=<path>` keeps it; frames from the
  `512x96stream-mock-frames` asset);
- `review_fixture` outputs (synthetic file with series, accounting and a stored
  record; `--population 3000`), both recorded at px→µm 0.25 so TD-17 is exercised.

What is compared, and what is not:

- **Exports** use the requests each shell builds. Qt's file dialogs are
  bypassed: the dump rebuilds the tab's `HdfExportRequest` (Export Metrics:
  explicit destination; Export All: the series prompt's default of all
  series). Chart TIFFs are left out, because the two shells render charts
  differently. Charts are compared through their data: point count, axis
  extents and histogram bins.
- **Histogram range**: the dump sets the Qt live processing config to the
  file's recorded one first. The Qt tab follows the live config by design (an
  accepted difference), so this compares the binning, not the range source.
- **Core record**: Qt computes and saves into a copy of the file; YOFO's
  ComputeCore record is compared with `computed_at_ns` ignored.
- Accepted differences are reported as ACCEPTED, with the reason recorded in
  the plan's decision log. See
  [YofoReview.md](../../knowledge_map/frontend/YofoReview.md) → "Differences
  from the Qt tab".
