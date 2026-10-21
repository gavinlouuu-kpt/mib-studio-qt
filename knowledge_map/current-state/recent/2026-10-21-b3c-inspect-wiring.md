## 2026-10-21 — R3 Inspect reads object coordinates, E-LUT, tracks and reprocess (#717 B3c)

Track B (#717) step B3c, on `feat/egrabber-workspace`: R3 Inspect (B3a)
wired to the review ABI 3 commands that B3b (#756) added. UI only, under
`desktop/src/review/`; no backend or contract change.

- **Object coordinate space.** The B3a stopgap (`cropNeedsCoordinateSpace`,
  Full frame only for a nonzero ROI origin) is gone. The crop places the
  stored box through `fetch_review_info` `object_coordinates`: `roi_local`
  at box + `object_origin_x/y`, `image` as stored; `unknown` gives no crop,
  with a plain reason. The Stored card's **Object coordinates** says the
  space and what decided it (`object_coordinates_source`). `cropSquare`
  stays bounds-safe and no longer cuts a box lying wholly outside the image.
- **E-LUT and profile.** `identity.emodulus_lut` and `profile_id` in the
  Stored card and the provenance line, "Not recorded" when absent (the
  "E-LUT Not available" placeholder is gone).
- **Linked cell records.** A new card: **Show linked records** sends
  `fetch_review_track_rows` (256 rows) for the selected record's track and
  lists them; a truncated track says so.
- **Reprocess one record.** **Reprocess this record** (recorded or default
  settings) sends `review_reprocess_record` and shows stored and
  reprocessed values side by side (the stored box placed in image
  coordinates), the match, and the settings and core. Never sent on
  navigation, selection, a sub-view, the set or a stage change; writes
  nothing; refusals are said plainly; a late reply is dropped. **Reprocess
  into a new file…** and the #754 wording are unchanged.
- Coordinator rulings on #774: a run whose snapshot records no E-LUT
  loaded shows its stored E as "Not computed (no E-LUT loaded)" (record
  card, comparison, records table, frame viewer), never "0.00 kPa"; legacy
  files keep E as stored. The reprocess reply names no E-LUT (it uses the
  app's current one, #764 item 2), so a recomputed E and the match line say
  it was "computed with the app's current E-LUT, which may differ from the
  run's". Track rows stay on their button.
- `reviewIpc.golden.json` gains one flow, "linked records and reprocess one
  record" (the two buttons' calls); no other flow changed.
- Docs: vault `frontend/YofoReview` (R3 Inspect), the manual
  `yofo-review.md`.
- Click-through on the review-only build against real files (a legacy
  capture with a moved ROI, which reads `unknown`; a regenerated file with a
  moved ROI, `image` by attribute; the legacy synthetic fixture; a v3
  capture with the `roi_local` attribute; the review-parity run, `image` by
  ROI origin): screenshots in
  `/mnt/hdd/shared/exports/workspace-walk-2026-10-21/b3c/`. No real capture
  with a moved ROI and a decided `roi_local` space exists yet; that case is
  covered by the panel and model tests.
