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
- `reviewIpc.golden.json` gains one flow, "linked records and reprocess one
  record" (the two buttons' calls); no other flow changed.
- Docs: vault `frontend/YofoReview` (R3 Inspect), the manual
  `yofo-review.md`.
