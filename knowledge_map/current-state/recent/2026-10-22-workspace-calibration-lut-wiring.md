## 2026-10-22 — Workspace calibration card reads the method report; Inspect names the reprocess E-LUT (A4b follow-up, B3d)

Two UI follow-ups on `feat/egrabber-workspace`, after the develop sync that brought bridge ABI 43
(#780) and review ABI 4 (#778). No backend or contract change.

- **Prepare Calibration card (#716 A4b, ABI 43).** The card reads the readiness report's
  `method_calibration` from the readiness read it already makes (no extra IPC). A calibration
  method reads "This method is a calibration method: closing its run derives a record", with its
  reference (diameter ± tolerance µm, CV limit %, nominal E when declared). Policy and max age come
  from the report. `declared: false` reads as a measurement method with policy none. A declaration
  `error` is shown beside the failing `calibration.record` gate. Only a `null` member (a backend
  before ABI 43, or the report not read) keeps the A4b wording and its purpose-gap note. Purpose and
  policy are never inferred from the gate's text.
- **R3 Inspect (#717 B3d, review ABI 4).** A recomputed E names the reply's `emodulus_lut` and the
  short `emodulus_lut_sha256`, instead of B3c's "computed with the app's current E-LUT" caveat.
  Where the run records its LUT hash, the two are compared by sha256: another hash (or a run that
  loaded none) marks the E row "different E-LUT: not comparable", and the match line says so. "Not
  recomputed (no E-LUT loaded)" stays. The B3c caveat stays only for a backend before review ABI 4,
  which sends neither field. `reviewIpc.golden.json` is unchanged.

See "Opt-in workspace" in [[architecture/Desktop-Shell]], [[services/CalibrationRecords]] and
"R3 Inspect" in [[frontend/YofoReview]].
