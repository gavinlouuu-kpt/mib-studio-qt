## 2026-10-19 — Notice for files from YOFO Review's old Regenerate masks (#737)

YOFO Review up to and including review-v1.1.3-beta.1 paired the records of Regenerate masks
with source frames by position. Most records in those files name the wrong frame. #735
(#512 S8) fixed this by moving regenerate onto the shared reanalysis engine.

This change records the notice:
- in the user manual (`docs/manual/yofo-review.md`: Regenerate masks, and an entry under
  Troubleshooting);
- as a pending notice in `docs/release-notes/README.md`, so the next YOFO Review release
  carries it in its notes.

No code change. See [[frontend/YofoReview]].
