## 2026-10-20 — Parity harness: a MIB Studio half (#512 S6)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S6. `tools/review_parity/` now measures MIB Studio as well as the Qt tab
and YOFO Review. It has a new cargo example,
`crates/mib-bridge/examples/review_parity_studio.rs`, which drives the
facade commands over `BackendBridge`:

- `load_recording`;
- `fetch_review_charts_json`, as opened and with the live factor and ring
  range set to the recorded ones;
- `review_export_json` metrics_csv and all, with `exportControls.tsx`'s
  defaults;
- `render_review_overlay`.

It writes `studio/`. The YOFO half also dumps its `fetch_review_frame`
overlays. `compare.py` reports Qt ↔ YOFO (unchanged), MIB Studio ↔ Qt and
MIB Studio ↔ YOFO. Decisions D2–D5 are ACCEPTED until S13, each only under
the condition its wording states. The ROI pixels, for example, must be the
two styles' rectangles. Chart ranges involving MIB Studio pass within 1e-12 of
the range's largest end. This covers long double against double padding: on
`population_3000`, YOFO is 1 ulp off.

On the four sign-off inputs, MIB Studio is identical to Qt and YOFO in:

- metrics.csv;
- Export All, apart from its backend-rendered chart TIFFs (D5);
- point counts, extents and histogram bins;
- the ROI-off overlays.

ROI-on overlays differ only on the outlines (D2). The as-opened px→µm differs
on the 0.25 µm/px fixtures (D3). Outputs are in
`/mnt/hdd/developer-data/mib-studio-qt/review-parity/out-2026-10-20-s6/`.

One open finding is not accepted: MIB Studio's histogram y axis tops at the
largest bin. Qt rounds it with `applyNiceNumbers()`, and YOFO uses
`ceil(1.1 × max)`. Until the coordinator rules, `run.sh` exits 1 on experiment
files.

`scripts.review_parity_compare` unit-tests the classifier. No contract change.
See [[YofoReview]].
