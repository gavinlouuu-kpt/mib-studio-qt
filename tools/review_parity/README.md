# Review parity harness (Qt Review tab, YOFO Review, MIB Studio)

Evidence for the parity sign-off of
[`2026-10-01-standalone-review-app.md`](../../docs/exec-plans/active/2026-10-01-standalone-review-app.md)
(PR 8, Qt ↔ YOFO) and for #512's "one review implementation"
([`2026-10-18-one-review-implementation.md`](../../docs/exec-plans/active/2026-10-18-one-review-implementation.md),
slice S6: the MIB Studio half). For each input file the harness records what
each shell shows and exports, then compares:

| Half | Program | Writes `<out>/<stem>/…` |
|---|---|---|
| Qt tab | `tests/frontend/review_parity_qt_dump.cpp` (ctest `parity.review_qt_dump`, offscreen `HdfReviewTab`) | `qt/summary.json` (status text, px→µm, scatter count + axis ranges, histogram bins/labels/y range), `qt/metrics.csv`, `qt/all/`, `qt/core.json` |
| YOFO Review | `crates/mib-bridge/examples/review_parity.rs` (the review bridge, as the app calls it) | `yofo/summary.json`, `yofo/scatter.json`, `yofo/metrics.csv`, `yofo/all/`, `yofo/core.json`, `yofo/overlays/` (`fetch_review_frame` with the overlay composed, PGM / PPM) |
| YOFO Review TS | `desktop/scripts/review-parity-charts.test.ts` (vitest, opt-in) | `yofo/charts.json`: `paddedExtent`, `ringRatioHistogram`, histogram y range |
| MIB Studio | `crates/mib-bridge/examples/review_parity_studio.rs` (the backend bridge `BackendBridge`, the facade commands MIB Studio's Review tab and YOFO Studio's remote client call) | `studio/summary.json`, `studio/charts_app.json`, `studio/charts.json` (`fetch_review_charts_json`), `studio/metrics.csv` and `studio/all/` (`review_export_json` metrics_csv / all), `studio/overlays/` (`render_review_overlay` PNGs) |
| Compare | `compare.py` (unit test `tests/tools/test_review_parity_compare.py`, ctest `scripts.review_parity_compare`) | `report.md`: three tables (Qt ↔ YOFO, MIB Studio ↔ Qt, MIB Studio ↔ YOFO), PASS / ACCEPTED / DIFF per check |

```bash
cmake --preset linux-system-release && cmake --build build/linux-system --target mib_frontend_tests
cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build --target mib_backend mib_review_core mib_processing
(cd desktop && npm ci)
tools/review_parity/run.sh <out-dir> <file.h5> [<file.h5> …]   # exit 1 on an unaccepted DIFF
```

`run.sh` builds both cargo examples with `MIB_BRIDGE_NO_CMAKE=1`: YOFO's with
`--features review-only`, MIB Studio's without it (it links `mib_backend`).
Under `review-only` the MIB Studio example compiles to a stub that exits 2, so
`cargo test --features review-only` still builds every example. Bridge CI's
`cargo test` builds both examples; nothing in CI runs them (they need the
sign-off inputs).

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
  series). MIB Studio's are the `review_export_json` requests
  `exportControls.tsx` builds with its defaults (frames both, all series,
  isoelastic overlays, keep partial, the live factor). Chart TIFFs are left
  out of the Qt and YOFO exports, because those shells render charts
  differently. Charts are compared through their data: point count, axis
  extents and histogram bins.
- **Histogram range**: the dump sets the Qt live processing config to the
  file's recorded one first. The Qt tab follows the live config by design (an
  accepted difference), so this compares the binning, not the range source.
- **Core record**: Qt computes and saves into a copy of the file; YOFO's
  ComputeCore record is compared with `computed_at_ns` ignored.
- Accepted Qt ↔ YOFO differences are reported as ACCEPTED, with the reason
  recorded in the plan's decision log. See
  [YofoReview.md](../../knowledge_map/frontend/YofoReview.md) → "Differences
  from the Qt tab".

## The MIB Studio half (#512 S6)

The studio half charts each file three times through the facade:

- **As opened** (`studio/charts_app.json`, `summary.json` `app`): the live
  px→µm and ring-ratio range of a freshly initialized backend (0.4886, 15–25),
  which is what MIB Studio's charts and exports use today.
- **D4 probe** (`studio/charts_d4.json`, `summary.json` `d4_probe`): the live
  ring-ratio range set to the recorded one ± 5, so that every input exercises
  D4. The sign-off inputs all record 15–25, the live default.
- **Compared outputs** (`studio/charts.json`, `metrics.csv`, `all/`,
  `overlays/`; `summary.json` `compared`): the live factor and range are first
  set to the file's recorded ones. Those are read through the review bridge,
  the reader the Qt tab and YOFO use; a file without them keeps the live
  values. This is the way the Qt dump sets its live config. The outputs then
  share the other halves' inputs, so any difference is in the implementation,
  not in the factor or range policy.

The reference points for the D3, D4 and density checks are YOFO's
`yofo/scatter.json` (area, deformability, ring ratio per valid row); the
Qt ↔ YOFO table holds them equal to Qt's.

These differences are accepted until #512 S13 (plan decisions D2–D5 and D10).
Each one is checked by its effect, not only by the input it declares; anything
else that differs is a DIFF:

| Decision | Check | Accepted when |
|---|---|---|
| D2 One ROI style | overlays, ROI on (studio ↔ YOFO) | Both halves report the same recorded ROI, and each draws its own style at that rectangle. MIB Studio: yellow (255, 210, 0) on the rectangle's 1 px path, `Rect(roi) & image`. YOFO: red within ±2 px of the path of `drawRoiOverlay`'s clamped rectangle; `cv::rectangle` thickness 3 is a 5 px band in OpenCV's LINE_8 raster. Every path pixel must carry the colour. A half that draws nothing, or draws elsewhere, is a DIFF. The halves may differ only on those pixels, and no difference at all is a PASS. The ROI-off renders must be pixel-identical. |
| D3 Factor policy | px→µm as opened | The as-opened charts declare the live factor, it differs from the recorded one, and their area extents equal the compared pass's × (live / compared)², with the deformability extents and the point count unchanged. The compared outputs must use the recorded factor. |
| D4 Histogram range | histogram range as opened; histogram range, live ≠ recorded (D4 probe) | The chart declares the live range, it differs from the other half's, and its bins are YOFO's ring ratios binned over it (0.5 wide, clamped into the edge bins). With equal ranges the row reads "D4 not exercised" and still checks the bins. |
| D5 Exported chart files | Export All chart files | MIB Studio's Export All holds exactly `scatter_plot.tiff` and `histogram.tiff` (the backend-rendered charts) and the other half none; every other file must be byte-identical. |
| D10 Histogram y-axis headroom (coordinator ruling 2026-10-20) | histogram y range | The bins are identical, and each y top is its shell's rule: MIB Studio `max(1, max bin)` (`desktop/src/components/ReviewCharts.tsx`, `maxBin`), YOFO `max(1, ceil(1.1 × max))`, Qt `applyNiceNumbers()` over that (`HdfReviewTab.cpp`; Qt's `looseNiceNumbers` with 5 ticks). |

**Density.** For studio ↔ YOFO, the 128 × 128 cells are compared cell by
cell with YOFO's points binned on MIB Studio's grid (its extents). A point
within 1e-9 of an interior cell edge may fall on either side, because the
facade bins in long double. Studio ↔ Qt compares the total.

**Chart-range tolerance.** `makeReviewChartData` pads the scatter extents in
`long double` (80-bit on Linux GCC, 64-bit on MSVC and arm64 macOS), while
YOFO's `chartMath.paddedExtent` pads in `double`. So the same data can give
neighbouring doubles; on `population_3000` the x and y minima differ by 1 ulp.
Ranges involving MIB Studio pass when every end agrees within
`CHART_RANGE_REL` = 1e-12 of the range's largest absolute end. That is about
4500 ulp, while the padding is three operations and differs by a few ulp at
most. Qt's ring-ratio range is read back from its bar labels, so it is
compared at their resolution (0.05). The Qt ↔ YOFO table keeps its 2026-10-04
tolerance (relative 1e-9).

Not compared for MIB Studio:

- overlays against Qt: the Qt dump has no overlay hook in the frozen tab, and
  `frontend.review_parity` pins the Qt QImage to the facade PNG without ROI;
- the accounting and status text;
- the core record, which MIB Studio's tab does not compute.
