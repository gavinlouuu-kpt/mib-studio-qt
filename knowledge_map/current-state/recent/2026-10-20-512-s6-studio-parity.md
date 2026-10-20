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
overlays and the recorded ROI. `compare.py` reports three tables: Qt ↔ YOFO
(unchanged), MIB Studio ↔ Qt and MIB Studio ↔ YOFO.

Decisions D2–D5 and D10 are ACCEPTED until S13, each checked by its effect
(Codex review of #755):

- **D2:** each half's ROI style must sit at the recorded ROI.
- **D3:** the area extents must scale by the factor ratio squared.
- **D4:** the bins must be the live range's. A D4 probe pass exercises it on
  every input.
- **D10:** the bins must be identical and each y top its shell's rule.

The scatter density is compared cell by cell. Chart ranges involving MIB
Studio pass within 1e-12 of the range's largest end: long double against
double padding puts YOFO 1 ulp off on `population_3000`.

On the four sign-off inputs, `run.sh` exits 0. MIB Studio is identical to Qt
and YOFO in:

- metrics.csv;
- Export All, apart from its backend-rendered chart TIFFs (D5);
- point counts, density cells, extents and histogram bins;
- the ROI-off overlays.

D10 is the coordinator's ruling of 2026-10-20 (called D6 in the ruling, which
is taken in the plan). MIB Studio's inline histogram y axis ends at the largest
bin, Qt applies nice numbers, and YOFO uses `ceil(1.1 × max)`. This is display
headroom only.

Outputs are in
`/mnt/hdd/developer-data/mib-studio-qt/review-parity/out-2026-10-20-s6-r2/`.
`scripts.review_parity_compare` unit-tests the classifier. No contract change.
See [[YofoReview]].
