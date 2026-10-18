## 2026-10-19 — Charts JSON and saved-overlay builders in the review core (#512 S4)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S4. The bodies of the facade's `fetch_review_charts_json` (the shared
chart math, the 128 × 128 density, histogram, bundled curves and the JSON) and
`render_review_overlay` (independent reader, 64 MiB limit, classification,
overlay, ROI, PNG) moved verbatim into `mib_review_core` as
`backend/review/ReviewCharts.h` (`reviewChartsJson`, `reviewChartsErrorJson`,
`renderSavedOverlayPng`). `BackendFacade` keeps the policy: the loaded file,
the live factor and ring-ratio range, and its yellow 1 px ROI
(`RoiStyle::Facade`). `RoiStyle::Review` draws the Qt / ReviewSession ROI and is
proven pixel-equal to `ReviewSession::fetchImage`, ready for D2's switch. New
test `review.charts_json` compares the builders byte for byte with the
pre-slice facade code (kept as the oracle) on 20 chart cases and 852 overlay
requests. Per the D3 ruling, `bridge-contract.json` drops the stale "same as
Qt" from `review_charts_json.calibration` (text only, no ABI bump). See
[[ReviewSession]], [[HdfExportService]].
