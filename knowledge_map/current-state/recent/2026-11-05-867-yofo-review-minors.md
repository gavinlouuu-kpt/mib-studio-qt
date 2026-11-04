## 2026-11-05 — YOFO Review minor Qt-parity rows (#867, review area)

The standalone YOFO Review module (`desktop/src/review/`) now matches the Qt Review tab on twelve
minor rows of the parity matrix; no review ABI, Rust or C++ change. Thumbnail tooltip `Frame N` +
`Double-click to view details` (REVIEW-014). Valid and Invalid tabs keep their own selection and
thumbnail scroll position (018, `tabMemory.ts`). Metrics number formats are the Qt model's:
deformability 3 dp, area 2 dp, ratios 3 dp, E 3 dp and blank when NaN, Yes / No (020). The frame
viewer follows `FrameViewerDialog`: Left/Up previous and Right/Down next frame with wrap-around
(series on its buttons and Shift+Up/Down, position 0 = the trigger image) (026), the Qt info header
(027), an overlay combo and ROI Overlay check that also drive the toolbar (028), Zoom Out / Fit to
Window / Zoom In and Ctrl+wheel (029), and Export as TIFF… as `frame_N[_series_K].tiff` through the
chart staging and the Export Charts job (031). The Charts frame pane has Qt's title (`Frame N ·
valid row k of n`, plus the failed-validation note), Previous / Next / Fit to Window / Open in
window… (032); the KDE core share is a Preferences setting (default 0.9, clamp 0.05 – 1) that the
density request and Compute core contour use instead of the stored record's fraction (036/037);
the scatter | (pane over histogram) layout has draggable splits remembered in `localStorage`
(039). Three silent failures now speak: isoelastic curves, the viewer's series count and Show in
folder. The IPC golden changed on purpose (viewer keys and series positions, the core share
argument, one new export-frame flow). See [[frontend/YofoReview]] and `docs/manual/yofo-review.md`.
